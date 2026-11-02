#include "progressive_jpeg.h"

#include <algorithm>
#include <cstring>
#include <climits>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {
constexpr size_t kDecodeBudget = 8 * 1024 * 1024; // 单次渐进式解码临时 PSRAM 上限
constexpr size_t kPsramReserve = 2 * 1024 * 1024; // 为显示及其他业务保留的 PSRAM
StaticSemaphore_t s_mutex_storage;
SemaphoreHandle_t DecodeMutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    return mutex;
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
    SemaphoreHandle_t mutex = DecodeMutex();
    if (mutex == nullptr) {
        if (error) *error = ImageDecodeError::kNoMemory;
        return false;
    }
    while (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        if (cancelled != nullptr && cancelled(context)) {
            if (error) *error = ImageDecodeError::kCancelled;
            return false;
        }
    }
    DecodeContext ctx{cancelled, context, data, len};
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    xSemaphoreGive(mutex);
    if (error) *error = ok ? ImageDecodeError::kNone :
        cancelled && cancelled(context) ? ImageDecodeError::kCancelled :
        ctx.no_memory ? ImageDecodeError::kNoMemory : ImageDecodeError::kInvalidData;
    return ok;
}
} // namespace reader
