#include "progressive_jpeg.h"
#include "progressive_jpeg_priv.h"

#include <algorithm>
#include <cstring>
#include <climits>
#include <new>
#include <memory>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {
constexpr size_t kDecodeBudget = reader::jpeg::kDecodeBudget; // 单次渐进式解码预算
constexpr size_t kPsramReserve = reader::jpeg::kPsramReserve; // 显示及其他业务预留
StaticSemaphore_t s_mutex_storage;
SemaphoreHandle_t DecodeMutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    return mutex;
}
class DecodeLock {
public:
    DecodeLock(reader::ImageDecodeCancelled cancelled, void* context) {
        mutex_ = DecodeMutex();
        if (!mutex_) return;
        while (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (cancelled && cancelled(context)) return;
        }
        acquired_ = true;
    }
    ~DecodeLock() { if (acquired_) xSemaphoreGive(mutex_); }
    explicit operator bool() const { return acquired_; }
private:
    SemaphoreHandle_t mutex_ = nullptr; // 共享解码互斥量
    bool acquired_ = false; // 当前调用已取得互斥量
};

struct ProgressiveInfo {
    int width = 0; // 原图宽度
    int height = 0; // 原图高度
    uint64_t bytes = 0; // stb 完整系数、平面及输出的保守峰值
};

bool ReadInfo(const uint8_t* data, FILE* input, size_t len, ProgressiveInfo& info,
              reader::ImageDecodeCancelled cancelled = nullptr, void* user = nullptr) {
    const auto read = [&](size_t offset, uint8_t* bytes, size_t count) {
        if (cancelled && cancelled(user)) return false;
        if (offset > len || count > len - offset) return false;
        if (data) { std::memcpy(bytes, data + offset, count); return true; }
        return input && offset <= LONG_MAX && std::fseek(input, offset, SEEK_SET) == 0 &&
            std::fread(bytes, 1, count, input) == count;
    };
    uint8_t frame[17];
    if (!read(0, frame, 2) || frame[0] != 255 || frame[1] != 0xd8) return false;
    size_t pos = 2;
    unsigned markers = 0;
    while (pos < len) {
        if ((++markers & 63) == 0) vTaskDelay(1);
        if (!read(pos++, frame, 1) || frame[0] != 255) return false;
        do { if (!read(pos++, frame, 1)) return false; } while (frame[0] == 255);
        const unsigned marker = frame[0];
        if (marker == 0xda || marker == 0xd9 || marker == 0xc0 || marker == 0xc1) return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (!read(pos, frame, 2)) return false;
        const size_t size = (unsigned(frame[0]) << 8) | frame[1];
        if (size < 2 || size > len - pos) return false;
        if (marker == 0xc2) {
            if (size < 8 || !read(pos + 2, frame, 6)) return false;
            const unsigned n = frame[5];
            if (frame[0] != 8 || (n != 1 && n != 3) || size != 8 + 3 * n ||
                !read(pos + 2, frame, 6 + 3 * n)) return false;
            info.height = (unsigned(frame[1]) << 8) | frame[2];
            info.width = (unsigned(frame[3]) << 8) | frame[4];
            unsigned max_h = 1, max_v = 1;
            for (unsigned i = 0; i < n; ++i) {
                const unsigned h = frame[7 + 3 * i] >> 4, v = frame[7 + 3 * i] & 15;
                if (!h || h > 4 || !v || v > 4) return false;
                max_h = std::max(max_h, h); max_v = std::max(max_v, v);
            }
            const uint64_t columns = (info.width + max_h * 8 - 1) / (max_h * 8);
            const uint64_t rows = (info.height + max_v * 8 - 1) / (max_v * 8);
            info.bytes = 32768 + uint64_t(info.width) * info.height;
            for (unsigned i = 0; i < n; ++i)
                info.bytes += columns * rows * (frame[7 + 3 * i] >> 4) * (frame[7 + 3 * i] & 15) * 64 * 3;
            return info.width > 0 && info.height > 0;
        }
        pos += size;
    }
    return false;
}
struct DecodeContext {
    reader::ImageDecodeCancelled cancelled; // 调用方取消条件
    void* user; // 调用方状态
    const uint8_t* data; // 借用输入 JPEG
    size_t length; // 输入长度
    size_t position = 0; // 输入游标
    size_t used = 0; // 当前已分配字节数
    size_t limit = 0; // 本次可用预算
    bool failed = false; // 取消或预算不足后不再读取
    bool no_memory = false; // 分配失败或超出 PSRAM 预算
    TickType_t last_yield = 0; // 防止持续解码饿死同核空闲任务
};
DecodeContext* s_decode = nullptr; // 仅在持有解码互斥量期间访问
struct alignas(16) Allocation {
    size_t size; // 含分配头的字节数
};
bool Cancelled(DecodeContext& ctx) {
    return ctx.failed || (ctx.cancelled != nullptr && ctx.cancelled(ctx.user));
}
void* JpegMalloc(size_t size) {
    auto& ctx = *s_decode;
    if (Cancelled(ctx)) return nullptr;
    if (size > ctx.limit || sizeof(Allocation) > ctx.limit - size ||
        size + sizeof(Allocation) > ctx.limit - ctx.used) {
        ctx.failed = true;
        ctx.no_memory = true;
        return nullptr;
    }
    const size_t bytes = sizeof(Allocation) + size;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free < kPsramReserve || bytes > free - kPsramReserve) {
        ctx.failed = true;
        ctx.no_memory = true;
        return nullptr;
    }
    auto* allocation = static_cast<Allocation*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (allocation == nullptr) {
        ctx.failed = true;
        ctx.no_memory = true;
        return nullptr;
    }
    allocation->size = bytes;
    ctx.used += bytes;
    return allocation + 1;
}
void JpegFree(void* ptr) {
    if (ptr == nullptr) return;
    auto* allocation = static_cast<Allocation*>(ptr) - 1;
    s_decode->used -= allocation->size;
    heap_caps_free(allocation);
}
__attribute__((unused)) void* JpegRealloc(void* ptr, size_t size) {
    if (ptr == nullptr) return JpegMalloc(size);
    auto* allocation = static_cast<Allocation*>(ptr) - 1;
    void* next = JpegMalloc(size);
    if (next != nullptr) {
        std::memcpy(next, ptr, std::min(size, allocation->size - sizeof(Allocation)));
        JpegFree(ptr);
    }
    return next;
}
int Read(void* user, char* out, int size) {
    auto& ctx = *static_cast<DecodeContext*>(user);
    if (Cancelled(ctx) || size <= 0) { ctx.failed = true; return 0; }
    const TickType_t now = xTaskGetTickCount();
    if (now - ctx.last_yield >= pdMS_TO_TICKS(20)) {
        vTaskDelay(1);
        ctx.last_yield = xTaskGetTickCount();
    }
    const size_t count = std::min(static_cast<size_t>(size), ctx.length - ctx.position);
    std::memcpy(out, ctx.data + ctx.position, count);
    ctx.position += count;
    return static_cast<int>(count);
}
void Skip(void* user, int size) {
    auto& ctx = *static_cast<DecodeContext*>(user);
    const int64_t position = static_cast<int64_t>(ctx.position) + size;
    if (position < 0 || static_cast<uint64_t>(position) > ctx.length) {
        ctx.failed = true;
        return;
    }
    ctx.position = static_cast<size_t>(position);
}
int Eof(void* user) {
    auto& ctx = *static_cast<DecodeContext*>(user);
    return Cancelled(ctx) || ctx.position == ctx.length;
}
} // namespace

#define STB_IMAGE_STATIC
#define STBIDEF static __attribute__((unused))
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 65535
#define STBI_MALLOC(size) JpegMalloc(size)
#define STBI_REALLOC(ptr, size) JpegRealloc(ptr, size)
#define STBI_FREE(ptr) JpegFree(ptr)
// 仅启用 JPEG 后，上游仍声明被裁掉功能的静态接口。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace reader {
bool IsProgressiveJpegFile(FILE* input, size_t len, ImageDecodeCancelled cancelled, void* context) {
    ProgressiveInfo info;
    return ReadInfo(nullptr, input, len, info, cancelled, context);
}
bool IsProgressiveJpeg(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    size_t pos = 2;
    while (pos < len) {
        if (data[pos++] != 0xff) return false;
        while (pos < len && data[pos] == 0xff) ++pos;
        if (pos == len) return false;
        const uint8_t marker = data[pos++];
        if (marker == 0xda || marker == 0xd9) return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (len - pos < 2) return false;
        const size_t size = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        if (size < 2 || size > len - pos) return false;
        if (marker == 0xc2) return size >= 8;
        pos += size;
    }
    return false;
}

bool DecodeProgressiveJpeg(const uint8_t* data, size_t len, int max_w, int max_h,
                           RasterImage& out, ImageDecodeCancelled cancelled, void* context,
                           ImageDecodeError* error) {
    out.Reset();
    if (error) *error = ImageDecodeError::kInvalidArgument;
    if (data == nullptr || len > INT_MAX || max_w <= 0 || max_h <= 0) return false;
    DecodeLock lock(cancelled, context);
    if (!lock) {
        if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled : ImageDecodeError::kNoMemory;
        return false;
    }
    DecodeContext ctx{cancelled, context, data, len};
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ProgressiveInfo info;
    // 缺少直接结束标记时使用严格解析，避免 stb 将截断扫描当作完整图片。
    const bool ended = len >= 2 && data[len - 2] == 0xff && data[len - 1] == 0xd9;
    if (!ended || (ReadInfo(data, nullptr, len, info, cancelled, context) &&
        (info.bytes > kDecodeBudget || free < kPsramReserve || info.bytes > free - kPsramReserve))) {
        return jpeg::DecodeProgressiveFallback(data, nullptr, len, max_w, max_h, out, cancelled, context, error);
    }
    ctx.limit = std::min(kDecodeBudget, free > kPsramReserve ? free - kPsramReserve : 0);
    s_decode = &ctx;
    int width = 0, height = 0, components = 0;
    const stbi_io_callbacks io{Read, Skip, Eof};
    bool ok = !Cancelled(ctx) && stbi_info_from_callbacks(&io, &ctx, &width, &height, &components) &&
        width > 0 && height > 0 &&
        (components == 1 || components == 3);
    ctx.position = 0;
    uint8_t* gray = ok ? stbi_load_from_callbacks(&io, &ctx, &width, &height, &components, 1) : nullptr;
    ok = gray != nullptr && !Cancelled(ctx);
    if (ok) {
        const double scale = std::min({1.0, double(max_w) / width, double(max_h) / height});
        const int w = std::max(1, int(width * scale));
        const int h = std::max(1, int(height * scale));
        // 沿用 RasterImage 的 L8 容器；拒绝大块像素落入内部 RAM。
        const size_t bytes = static_cast<size_t>(w) * h;
        const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = bytes <= ctx.limit - ctx.used && psram >= kPsramReserve && bytes <= psram - kPsramReserve &&
            bytes <= heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ok) ctx.no_memory = true;
        if (ok) {
            try {
                out.pixels.resize(bytes);
            } catch (const std::bad_alloc&) {
                ctx.failed = true;
                ctx.no_memory = true;
            }
            if (bytes > 4096 && !esp_ptr_external_ram(out.pixels.data())) {
                ctx.failed = true;
                ctx.no_memory = true;
            }
            for (int y = 0; y < h && !Cancelled(ctx); ++y) {
                for (int x = 0; x < w; ++x) {
                    out.pixels[static_cast<size_t>(y) * w + x] =
                        gray[static_cast<size_t>(int64_t(y) * height / h) * width + int64_t(x) * width / w];
                }
                if ((y & 31) == 31) vTaskDelay(1);
            }
            ok = !Cancelled(ctx);
            if (ok) { out.width = w; out.height = h; out.BindDsc(); }
        }
    }
    stbi_image_free(gray);
    if (!ok && ctx.no_memory && !(cancelled && cancelled(context))) {
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
        s_decode = nullptr;
        return jpeg::DecodeProgressiveFallback(data, nullptr, len, max_w, max_h, out, cancelled, context, error);
    }
    if (!ok) {
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
        if (cancelled == nullptr || !cancelled(context)) {
            ESP_LOGW("ProgressiveJpeg", "decode rejected/failed %dx%d bytes=%u budget=%u: %s",
                     width, height, unsigned(len), unsigned(ctx.limit),
                     ctx.failed ? "PSRAM budget/read failure" : "invalid/truncated JPEG or size/format limit");
        }
    }
    s_decode = nullptr;
    if (error) *error = ok ? ImageDecodeError::kNone :
        cancelled && cancelled(context) ? ImageDecodeError::kCancelled :
        ctx.no_memory ? ImageDecodeError::kNoMemory : ImageDecodeError::kInvalidData;
    return ok;
}

bool DecodeProgressiveJpegFile(FILE* input, size_t len, int max_w, int max_h,
                               RasterImage& out, ImageDecodeCancelled cancelled, void* context,
                               ImageDecodeError* error) {
    out.Reset();
    if (error) *error = ImageDecodeError::kInvalidArgument;
    if (!input || !len || len > LONG_MAX || !((max_w == 0 && max_h == 0) || (max_w > 0 && max_h > 0))) return false;
    if (cancelled && cancelled(context)) {
        if (error) *error = ImageDecodeError::kCancelled;
        return false;
    }
    ProgressiveInfo info;
    if (!ReadInfo(nullptr, input, len, info, cancelled, context)) {
        if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled :
            std::ferror(input) ? ImageDecodeError::kFileIo : ImageDecodeError::kInvalidData;
        return false;
    }
    if (max_w == 0) { max_w = info.width; max_h = info.height; }
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (info.bytes <= kDecodeBudget && free >= kPsramReserve && info.bytes <= free - kPsramReserve &&
        len <= free - kPsramReserve - info.bytes) {
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> bytes(
            static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
        if (bytes) {
            if (std::fseek(input, 0, SEEK_SET) != 0) {
                if (error) *error = ImageDecodeError::kFileIo;
                return false;
            }
            size_t read = 0;
            while (read < len && !(cancelled && cancelled(context))) {
                const size_t count = std::min<size_t>(16384, len - read);
                if (std::fread(bytes.get() + read, 1, count, input) != count) break;
                read += count;
            }
            if (read != len) {
                if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled : ImageDecodeError::kFileIo;
                return false;
            }
            ImageDecodeError result;
            const bool ok = DecodeProgressiveJpeg(bytes.get(), len, max_w, max_h, out, cancelled, context, &result);
            if (ok || result != ImageDecodeError::kNoMemory) {
                if (error) *error = result;
                return ok;
            }
        }
    }
    DecodeLock lock(cancelled, context);
    if (!lock) {
        if (error) *error = cancelled && cancelled(context) ? ImageDecodeError::kCancelled : ImageDecodeError::kNoMemory;
        return false;
    }
    return jpeg::DecodeProgressiveFallback(nullptr, input, len, max_w, max_h, out, cancelled, context, error);
}
} // namespace reader
