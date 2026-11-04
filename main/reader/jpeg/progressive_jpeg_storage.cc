#include "progressive_jpeg_priv.h"
#include "sd_paths.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <unistd.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reader::jpeg {
namespace {
constexpr size_t kWorkBudget = 3 * 1024 * 1024; // 条带及格式工作区上限，不含最终 L8

bool Position(FILE* file, uint32_t offset) {
    return file && offset <= LONG_MAX &&
        (std::ftell(file) == static_cast<long>(offset) || std::fseek(file, offset, SEEK_SET) == 0);
}

void CleanAbandonedWork() {
    static bool cleaned = false;
    if (cleaned) return;
    DIR* directory = opendir(SD_PATH_WALLPAPER_TEMP);
    if (!directory) return;
    cleaned = true;
    while (const dirent* entry = readdir(directory)) {
        const char* name = entry->d_name;
        if (std::strlen(name) != 15 || std::memcmp(name, "pj-", 3) || std::strcmp(name + 11, ".tmp")) continue;
        bool owned = true;
        for (unsigned i = 3; i < 11; ++i)
            if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) owned = false;
        if (!owned) continue;
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%.15s", SD_PATH_WALLPAPER_TEMP, name);
        if (unlink(path) != 0) ESP_LOGW("ProgressiveJpeg", "abandoned work cleanup failed: %s", path);
    }
    closedir(directory);
}

bool OpenWork(DecodeContext& ctx) {
    if (ctx.work) return true;
    ctx.transaction.emplace();
    if (!*ctx.transaction || !SdEnsureDir(SD_PATH_TEMP) || !SdEnsureDir(SD_PATH_WALLPAPER_TEMP)) {
        ctx.Fail(ImageDecodeError::kFileIo);
        return false;
    }
    CleanAbandonedWork();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::snprintf(ctx.work_path, sizeof(ctx.work_path), "%s/pj-%08lx.tmp",
                      SD_PATH_WALLPAPER_TEMP, static_cast<unsigned long>(esp_random()));
        const int fd = open(ctx.work_path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            ctx.work_path[0] = '\0';
            if (errno == EEXIST) continue;
            break;
        }
        ctx.work = fdopen(fd, "w+b");
        if (!ctx.work) close(fd);
        if (ctx.work) setvbuf(ctx.work, nullptr, _IONBF, 0);
        break;
    }
    if (!ctx.work) ctx.Fail(ImageDecodeError::kFileIo);
    return ctx.work != nullptr;
}
} // namespace

DecodeContext::~DecodeContext() {
    if (work && std::fclose(work) != 0) ESP_LOGW("ProgressiveJpeg", "work file close failed");
    if (work_path[0] && unlink(work_path) != 0)
        ESP_LOGW("ProgressiveJpeg", "work file cleanup failed: %s", work_path);
    heap_caps_free(work_cache);
    heap_caps_free(arena);
}

bool DecodeContext::Cancelled() {
    if (cancelled && cancelled(user)) Fail(ImageDecodeError::kCancelled);
    const TickType_t now = xTaskGetTickCount();
    if (now - last_yield >= pdMS_TO_TICKS(20)) {
        vTaskDelay(1);
        last_yield = xTaskGetTickCount();
    }
    return error != ImageDecodeError::kNone;
}

bool DecodeContext::Read(uint32_t offset, void* bytes, size_t size) {
    if (Cancelled()) return false;
    if (offset > input_size || size > input_size - offset) {
        Fail(ImageDecodeError::kInvalidData);
        return false;
    }
    if (data) {
        std::memcpy(bytes, data + offset, size);
        return true;
    }
    if (!Position(input, offset) || std::fread(bytes, 1, size, input) != size) {
        Fail(input && !std::ferror(input) ? ImageDecodeError::kInvalidData : ImageDecodeError::kFileIo);
        return false;
    }
    return true;
}

void* DecodeContext::Allocate(size_t size) {
    if (Cancelled() || size > arena_size - used) {
        Fail(ImageDecodeError::kNoMemory);
        return nullptr;
    }
    void* memory = arena + used;
    used += (size + 15) & ~size_t(15);
    std::memset(memory, 0, size);
    return memory;
}

void DecodeContext::CacheWork(uint32_t bytes) {
    if (work_cache || !bytes || Cancelled()) return;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free <= kPsramReserve || output_bytes > free - kPsramReserve ||
        output_bytes > kDecodeBudget - arena_size) return;
    size_t wanted = std::min({size_t(bytes), kWorkBudget - arena_size,
        kDecodeBudget - arena_size - output_bytes, free - kPsramReserve - output_bytes, largest}) & ~size_t(511);
    while (wanted >= 32768) {
        work_cache = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (work_cache) {
            work_cache_size = wanted;
            std::memset(work_cache, 0, wanted);
            return;
        }
        wanted = (wanted / 2) & ~size_t(511);
    }
}

bool DecodeContext::Begin(uint16_t width, uint16_t height) {
    const size_t bytes = size_t(width) * height;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes || bytes > kDecodeBudget - arena_size - work_cache_size || free < kPsramReserve ||
        bytes > free - kPsramReserve ||
        bytes > heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        Fail(ImageDecodeError::kNoMemory);
        return false;
    }
    try {
        output->pixels.resize(bytes);
    } catch (const std::bad_alloc&) {
        Fail(ImageDecodeError::kNoMemory);
        return false;
    }
    if (bytes > 4096 && !esp_ptr_external_ram(output->pixels.data())) {
        Fail(ImageDecodeError::kNoMemory);
        return false;
    }
    output->width = width;
    output->height = height;
    return true;
}

bool DecodeContext::Output(uint32_t offset, const void* bytes, size_t size) {
    if (Cancelled()) return false;
    if (offset > output->pixels.size() || size > output->pixels.size() - offset) {
        Fail(ImageDecodeError::kInvalidData);
        return false;
    }
    std::memcpy(output->pixels.data() + offset, bytes, size);
    return true;
}

bool DecodeContext::WorkRead(uint32_t offset, void* bytes, size_t size) {
    if (Cancelled()) return false;
    if (offset > work_size || size > work_size - offset) {
        Fail(ImageDecodeError::kFileIo);
        return false;
    }
    if (offset < work_cache_size) {
        const size_t cached = std::min<size_t>(size, work_cache_size - offset);
        std::memcpy(bytes, work_cache + offset, cached);
        offset += cached;
        bytes = static_cast<uint8_t*>(bytes) + cached;
        size -= cached;
    }
    if (size && (!work || std::fseek(work, offset, SEEK_SET) != 0 || std::fread(bytes, 1, size, work) != size)) {
        Fail(ImageDecodeError::kFileIo);
        return false;
    }
    return true;
}

bool DecodeContext::WorkWrite(uint32_t offset, const void* bytes, size_t size) {
    if (Cancelled()) return false;
    if (offset > LONG_MAX || size > size_t(LONG_MAX) - offset) {
        Fail(ImageDecodeError::kNoMemory);
        return false;
    }
    const uint32_t end = offset + size;
    if (offset < work_cache_size) {
        const size_t cached = std::min<size_t>(size, work_cache_size - offset);
        std::memcpy(work_cache + offset, bytes, cached);
        offset += cached;
        bytes = static_cast<const uint8_t*>(bytes) + cached;
        size -= cached;
    }
    if (size && (!OpenWork(*this) || std::fseek(work, offset, SEEK_SET) != 0 || std::fwrite(bytes, 1, size, work) != size)) {
        Fail(ImageDecodeError::kFileIo);
        return false;
    }
    work_size = std::max(work_size, end);
    return true;
}

bool DecodeProgressiveFallback(const uint8_t* data, FILE* input, size_t size, int max_w, int max_h,
                               RasterImage& out, ImageDecodeCancelled cancelled, void* user,
                               ImageDecodeError* error) {
    DecodeContext ctx;
    ctx.data = data;
    ctx.input = input;
    ctx.input_size = size;
    ctx.max_width = max_w;
    ctx.max_height = max_h;
    ctx.cancelled = cancelled;
    ctx.user = user;
    ctx.output = &out;
    ctx.arena_size = ProgressiveWorkspaceSize();
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free > kPsramReserve && ctx.arena_size <= free - kPsramReserve)
        ctx.arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, ctx.arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ctx.arena) ctx.Fail(ImageDecodeError::kNoMemory);
    const bool ok = !ctx.Cancelled() && DecodePagedProgressive(ctx) && !ctx.Cancelled();
    if (ok) {
        out.BindDsc();
    } else {
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
        if (ctx.error != ImageDecodeError::kCancelled)
            ESP_LOGW("ProgressiveJpeg", "bounded decode failed bytes=%u error=%u", unsigned(size), unsigned(ctx.error));
    }
    if (error) *error = ok ? ImageDecodeError::kNone : ctx.error;
    return ok;
}
} // namespace reader::jpeg
