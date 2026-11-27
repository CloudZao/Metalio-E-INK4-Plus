#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <unistd.h>
#include "infra/storage/sd_data_layout.h"
#include "infra/storage/sd_paths.h"
#include "infra/storage/sd_work_file.h"
#include "progressive_jpeg_priv.h"

static constexpr size_t kWorkBudget = 3 * 1024 * 1024;  // 条带及格式工作区上限，不含最终 L8

static bool Position(FILE* file, uint32_t offset) {
    return file && offset <= LONG_MAX &&
           (std::ftell(file) == static_cast<long>(offset) || std::fseek(file, offset, SEEK_SET) == 0);
}

static bool OpenWork(JpegDecodeContext& ctx) {
    if (ctx.work) {
        return true;
    }
    ctx.transaction.emplace();
    if (!*ctx.transaction) {
        if (ctx.storage_error && !*ctx.storage_error) {
            *ctx.storage_error = ctx.transaction->Error();
        }
        ctx.Fail(ImageDecodeError::kFileIo, StorageStage::Access);
        return false;
    }
    if (!SdEnsureDirectory(SD_PATH_IMAGE_WORK)) {
        ctx.Fail(ImageDecodeError::kFileIo, StorageStage::CreateDirectory);
        return false;
    }
    static std::mutex cleanup_mutex;
    static bool cleaned = false;
    {
        std::lock_guard<std::mutex> lock(cleanup_mutex);
        if (!cleaned) {
            cleaned = SdDataCleanWorkFiles(SD_PATH_IMAGE_WORK, "pj-");
        }
        if (!cleaned) {
            ctx.Fail(ImageDecodeError::kFileIo);
            return false;
        }
    }
    const int fd = SdCreateWorkFile(SdWorkArea::Image, "pj-", ctx.work_path, sizeof(ctx.work_path));
    if (fd >= 0) {
        ctx.work = fdopen(fd, "w+b");
        if (!ctx.work) {
            ctx.Fail(ImageDecodeError::kFileIo, StorageStage::Open);
            close(fd);
        } else
            setvbuf(ctx.work, nullptr, _IONBF, 0);
    }
    if (!ctx.work) {
        ctx.Fail(ImageDecodeError::kFileIo, StorageStage::Open);
    }
    return ctx.work != nullptr;
}

JpegDecodeContext::~JpegDecodeContext() {
    if (work && std::fclose(work) != 0) {
        ESP_LOGW("ProgressiveJpeg", "work file close failed");
    }
    if (work_path[0] && unlink(work_path) != 0) {
        ESP_LOGW("ProgressiveJpeg", "work file cleanup failed errno=%d", errno);
    }
    heap_caps_free(work_cache);
    heap_caps_free(arena);
}

bool JpegDecodeContext::Cancelled() {
    if (cancelled && cancelled(user)) {
        Fail(ImageDecodeError::kCancelled);
    }
    const TickType_t now = xTaskGetTickCount();
    if (now - last_yield >= pdMS_TO_TICKS(20)) {
        vTaskDelay(1);
        last_yield = xTaskGetTickCount();
    }
    return error != ImageDecodeError::kNone;
}

bool JpegDecodeContext::Read(uint32_t offset, void* bytes, size_t size) {
    if (Cancelled()) {
        return false;
    }
    if (offset > input_size || size > input_size - offset) {
        Fail(ImageDecodeError::kInvalidData);
        return false;
    }
    if (data) {
        std::memcpy(bytes, data + offset, size);
        return true;
    }
    if (!Position(input, offset)) {
        Fail(ImageDecodeError::kFileIo, StorageStage::Seek);
        return false;
    }
    if (std::fread(bytes, 1, size, input) != size) {
        Fail(std::ferror(input) ? ImageDecodeError::kFileIo : ImageDecodeError::kInvalidData);
        return false;
    }
    return true;
}

void* JpegDecodeContext::Allocate(size_t size) {
    if (Cancelled() || size > arena_size - used) {
        Fail(ImageDecodeError::kNoMemory);
        return nullptr;
    }
    void* memory = arena + used;
    used += (size + 15) & ~size_t(15);
    std::memset(memory, 0, size);
    return memory;
}

void JpegDecodeContext::CacheWork(uint32_t bytes) {
    if (work_cache || !bytes || Cancelled()) {
        return;
    }
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free <= kJpegPsramReserve || output_bytes > free - kJpegPsramReserve ||
        output_bytes > kJpegDecodeBudget - arena_size) {
        return;
    }
    size_t wanted = std::min({size_t(bytes), kWorkBudget - arena_size, kJpegDecodeBudget - arena_size - output_bytes,
                              free - kJpegPsramReserve - output_bytes}) &
                    ~size_t(511);
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

bool JpegDecodeContext::Begin(uint16_t width, uint16_t height) {
    const size_t bytes = size_t(width) * height;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes || bytes > kJpegDecodeBudget - arena_size - work_cache_size || free < kJpegPsramReserve ||
        bytes > free - kJpegPsramReserve) {
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

bool JpegDecodeContext::Output(uint32_t offset, const void* bytes, size_t size) {
    if (Cancelled()) {
        return false;
    }
    if (offset > output->pixels.size() || size > output->pixels.size() - offset) {
        Fail(ImageDecodeError::kInvalidData);
        return false;
    }
    std::memcpy(output->pixels.data() + offset, bytes, size);
    return true;
}

bool JpegDecodeContext::WorkRead(uint32_t offset, void* bytes, size_t size) {
    if (Cancelled()) {
        return false;
    }
    if (offset > work_size || size > work_size - offset) {
        Fail(ImageDecodeError::kInvalidData);
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

bool JpegDecodeContext::WorkWrite(uint32_t offset, const void* bytes, size_t size) {
    if (Cancelled()) {
        return false;
    }
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
    if (size &&
        (!OpenWork(*this) || std::fseek(work, offset, SEEK_SET) != 0 || std::fwrite(bytes, 1, size, work) != size)) {
        Fail(ImageDecodeError::kFileIo, StorageStage::Write);
        return false;
    }
    work_size = std::max(work_size, end);
    return true;
}

bool JpegDecodeProgressiveFallback(const uint8_t* data, FILE* input, size_t size, int max_w, int max_h,
                                   RasterImage& out, ImageDecodeCancelled cancelled, void* user,
                                   ImageDecodeError* error, StorageError* storage_error) {
    StorageError local_error;
    JpegDecodeContext ctx;
    ctx.storage_error = storage_error ? storage_error : &local_error;
    ctx.data = data;
    ctx.input = input;
    ctx.input_size = size;
    ctx.max_width = max_w;
    ctx.max_height = max_h;
    ctx.cancelled = cancelled;
    ctx.user = user;
    ctx.output = &out;
    ctx.arena_size = JpegProgressiveWorkspaceSize();
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free > kJpegPsramReserve && ctx.arena_size <= free - kJpegPsramReserve) {
        ctx.arena =
            static_cast<uint8_t*>(heap_caps_aligned_alloc(16, ctx.arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!ctx.arena) {
        ctx.Fail(ImageDecodeError::kNoMemory);
    }
    const bool ok = !ctx.Cancelled() && JpegDecodePagedProgressive(ctx) && !ctx.Cancelled();
    if (ok) {
        out.BindDsc();
    } else {
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
        if (local_error) {
            LogStorageError("ProgressiveJpeg", local_error);
        } else if (!error && ctx.error != ImageDecodeError::kCancelled && ctx.error != ImageDecodeError::kFileIo) {
            ESP_LOGW("ProgressiveJpeg", "bounded decode failed bytes=%u error=%u", unsigned(size), unsigned(ctx.error));
        }
    }
    if (error) {
        *error = ok ? ImageDecodeError::kNone : ctx.error;
    }
    return ok;
}
