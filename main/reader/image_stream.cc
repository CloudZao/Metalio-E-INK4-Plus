#include "image_stream.h"
#include "image_geometry.h"
#include "image_util.h"

// xtensa-g++：本文件在 -Os 下可能 ICE（cfgcleanup try_forward_edges）
#pragma GCC optimize("O1")

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <climits>
#include <memory>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/tjpgd.h"
#define READER_HAS_TJPGD 1
#elif CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/tjpgd.h"
#define READER_HAS_TJPGD 1
#elif CONFIG_IDF_TARGET_ESP32S31
#include "esp32s31/rom/tjpgd.h"
#define READER_HAS_TJPGD 1
#else
#define READER_HAS_TJPGD 0
#endif

#include <zlib.h>

namespace reader {
namespace {

constexpr const char* TAG = "ImgStream";
constexpr size_t kChunkIo = 8 * 1024;
constexpr size_t kMaxPngOutput = 1024 * 1024; // 缩放后 L8 最大字节数
struct PngWorkspace {
    uint8_t io[kChunkIo]; // 压缩输入块
    uint8_t palette[256 * 4]; // RGBA 调色板
    alignas(16) uint8_t inflate[64 * 1024]; // zlib 状态及 32 KiB 窗口的固定内存池
    size_t used = 0; // zlib 内存池游标
};
constexpr size_t kPngWorkspaceBytes = sizeof(PngWorkspace); // 固定解压工作区，像素行另按实际宽度分配
StaticSemaphore_t s_png_mutex_storage;
SemaphoreHandle_t PngMutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_png_mutex_storage);
    return mutex;
}
struct PngLock {
    SemaphoreHandle_t mutex; // 当前请求独占的工作区锁
    ~PngLock() { xSemaphoreGive(mutex); }
};
voidpf PngZalloc(voidpf opaque, uInt count, uInt size) {
    auto* work = static_cast<PngWorkspace*>(opaque);
    if (size != 0 && count > sizeof(work->inflate) / size) return nullptr;
    const size_t bytes = static_cast<size_t>(count) * size;
    const size_t offset = (work->used + 15) & ~size_t(15);
    if (offset > sizeof(work->inflate) || bytes > sizeof(work->inflate) - offset) return nullptr;
    void* ptr = work->inflate + offset;
    std::memset(ptr, 0, bytes);
    work->used = offset + bytes;
    return ptr;
}
void PngZfree(voidpf, voidpf) {
    // 单次 inflate 生命周期结束后整体释放固定工作区。
}

uint8_t* AllocPs(size_t n) {
    uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
    }
    return p;
}

bool AllocRasterL8(RasterImage& out, int w, int h) {
    out.Reset();
    if (w <= 0 || h <= 0) {
        return false;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    try {
        out.pixels.assign(n, 0xFF);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (out.pixels.size() != n) {
        out.Reset();
        return false;
    }
    out.width = static_cast<uint16_t>(w);
    out.height = static_cast<uint16_t>(h);
    out.BindDsc();
    return true;
}

uint32_t RdBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool StreamReadExact(ZipReader& zip, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t r = zip.EntryStreamRead(dst + got, n - got);
        if (r == 0) {
            return false;
        }
        got += r;
    }
    return true;
}

bool StreamSkip(ZipReader& zip, size_t n) {
    uint8_t tmp[256];
    while (n > 0) {
        const size_t chunk = n > sizeof(tmp) ? sizeof(tmp) : n;
        if (!StreamReadExact(zip, tmp, chunk)) {
            return false;
        }
        n -= chunk;
    }
    return true;
}

bool AbortRequested(const std::atomic<bool>* abort) {
    return abort != nullptr && abort->load(std::memory_order_relaxed);
}

#if READER_HAS_TJPGD

struct JpegCtx {
    ZipReader* zip = nullptr;
    RasterImage* out = nullptr;
    const std::atomic<bool>* abort = nullptr;
    int src_w = 0;
    int src_h = 0;
    int scale = 0;
    int dec_w = 0;
    int dec_h = 0;
    int out_w = 0;
    int out_h = 0;
};

UINT JpegInFunc(JDEC* jd, BYTE* buff, UINT nbyte) {
    auto* ctx = static_cast<JpegCtx*>(jd->device);
    if (ctx == nullptr || ctx->zip == nullptr || AbortRequested(ctx->abort)) {
        return 0;
    }
    if (buff == nullptr) {
        uint8_t tmp[64];
        UINT left = nbyte;
        while (left > 0) {
            const UINT chunk = left > sizeof(tmp) ? sizeof(tmp) : left;
            const size_t r = ctx->zip->EntryStreamRead(tmp, chunk);
            if (r == 0) {
                return nbyte - left;
            }
            left -= static_cast<UINT>(r);
        }
        return nbyte;
    }
    return static_cast<UINT>(ctx->zip->EntryStreamRead(buff, nbyte));
}

UINT JpegOutFunc(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* ctx = static_cast<JpegCtx*>(jd->device);
    if (ctx == nullptr || ctx->out == nullptr || ctx->out->empty() || ctx->dec_w <= 0 ||
        ctx->dec_h <= 0) {
        return 0;
    }
    const BYTE* src = static_cast<const BYTE*>(bitmap);
    const int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; ++y) {
        const int dy = y * ctx->out_h / ctx->dec_h;
        if (dy < 0 || dy >= ctx->out_h) {
            src += static_cast<size_t>(rw) * 3;
            continue;
        }
        for (int x = rect->left; x <= rect->right; ++x) {
            const int dx = x * ctx->out_w / ctx->dec_w;
            const uint8_t r = src[0];
            const uint8_t g = src[1];
            const uint8_t b = src[2];
            src += 3;
            if (dx < 0 || dx >= ctx->out_w) {
                continue;
            }
            const uint8_t gray = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            ctx->out->pixels[static_cast<size_t>(dy) * ctx->out_w + dx] = gray;
        }
    }
    return 1;
}

bool DecodeJpegStream(ZipReader& zip, int max_w, int max_h, RasterImage& out,
                      const std::atomic<bool>* abort) {
    constexpr size_t kPool = 32768;
    void* pool = AllocPs(kPool);
    if (pool == nullptr) {
        ESP_LOGE(TAG, "decode=stream jpeg pool alloc fail");
        return false;
    }

    JpegCtx ctx{};
    ctx.zip = &zip;
    ctx.abort = abort;

    JDEC jdec{};
    JRESULT jr = jd_prepare(&jdec, JpegInFunc, pool, kPool, &ctx);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "decode=stream jpeg prepare err=%d%s", static_cast<int>(jr),
                 jr == JDR_FMT3 ? " (unsupported, e.g. progressive)" : "");
        heap_caps_free(pool);
        return false;
    }
    if (AbortRequested(abort)) {
        heap_caps_free(pool);
        out.Reset();
        return false;
    }

    ctx.src_w = static_cast<int>(jdec.width);
    ctx.src_h = static_cast<int>(jdec.height);

    // 先算目标拟合框，再选仍能盖住该框的最大 tjpgd scale（避免 1/8 解得比槽位还小）
    int fit_w = 0;
    int fit_h = 0;
    FitImageSize(ctx.src_w, ctx.src_h, max_w, max_h, &fit_w, &fit_h);
    int scale = 0;
    for (int s = 0; s <= 3; ++s) {
        const int ow = std::max(1, ctx.src_w >> s);
        const int oh = std::max(1, ctx.src_h >> s);
        if (ow >= fit_w && oh >= fit_h) {
            scale = s;
        } else {
            break;
        }
    }
    ctx.scale = scale;
    ctx.dec_w = std::max(1, ctx.src_w >> scale);
    ctx.dec_h = std::max(1, ctx.src_h >> scale);
    FitImageSize(ctx.dec_w, ctx.dec_h, max_w, max_h, &ctx.out_w, &ctx.out_h);
    if (!AllocRasterL8(out, ctx.out_w, ctx.out_h)) {
        heap_caps_free(pool);
        return false;
    }
    ctx.out = &out;

    ESP_LOGI(TAG, "decode=stream jpeg %dx%d scale=1/%d -> %dx%d", ctx.src_w, ctx.src_h, 1 << scale,
             ctx.out_w, ctx.out_h);
    jr = jd_decomp(&jdec, JpegOutFunc, static_cast<BYTE>(scale));
    heap_caps_free(pool);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "decode=stream jpeg decomp err=%d", static_cast<int>(jr));
        out.Reset();
        return false;
    }
    out.BindDsc();
    return true;
}

#endif

bool DecodeImageBuffered(ZipReader& zip, int max_w, int max_h, RasterImage& out,
                      const std::atomic<bool>* abort) {
    out.Reset();
    const size_t size = zip.EntryStreamUncompSize();
    if (!size || size > 4 * 1024 * 1024 || AbortRequested(abort)) return false;
    auto* compressed = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!compressed) return false;
    size_t read = 0;
    while (read < size && !AbortRequested(abort)) {
        const size_t n = zip.EntryStreamRead(compressed + read, std::min(kChunkIo, size - read));
        if (!n) break;
        read += n;
    }
    const bool ok = read == size && !AbortRequested(abort) && !zip.EntryStreamError() &&
        DecodeImageToL8(compressed, size, max_w, max_h, out, [](void* p) {
            return AbortRequested(static_cast<const std::atomic<bool>*>(p));
        }, const_cast<std::atomic<bool>*>(abort));
    heap_caps_free(compressed);
    ESP_LOGI(TAG, "decode=image software fit bytes=%u ok=%d", static_cast<unsigned>(size), ok);
    return ok;
}

struct PngStream {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t color_type = 0;
    uint8_t bit_depth = 8; // 原始通道位深
    const std::atomic<bool>* abort = nullptr; // ZIP 调用方取消标志
    ImageDecodeCancelled cancelled = nullptr; // 文件或内存调用方取消条件
    void* cancel_context = nullptr; // 取消条件状态
    PngWorkspace* work = nullptr; // 借用独占的固定工作区
    bool z_end = false; // 已完整校验 zlib 流尾
    int bpp = 0;
    int row_bytes = 0;

    z_stream z{};
    bool z_init = false;
    uint8_t* prev = nullptr;
    uint8_t* cur = nullptr;
    uint8_t* pal = nullptr; // 索引色 PLTE，每项 RGBA
    int pal_n = 0; // 调色板有效项数
    uint16_t transparent[3] = {}; // tRNS 透明色键，按灰度或 RGB 通道保存
    bool has_transparent = false; // 是否存在灰度或 RGB 透明色键
    uint32_t row_index = 0;
    size_t row_fill = 0;
};

void PngStreamFree(PngStream& s) {
    if (s.z_init) {
        inflateEnd(&s.z);
        s.z_init = false;
    }
    s.prev = s.cur = s.pal = nullptr;
}

uint8_t Paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

bool PngUnfilterRow(uint8_t* row, const uint8_t* prev, int row_bytes, int bpp, uint8_t filter) {
    uint8_t* data = row + 1;
    const int n = row_bytes;
    switch (filter) {
        case 0:
            break;
        case 1:
            for (int i = bpp; i < n; ++i) {
                data[i] = static_cast<uint8_t>(data[i] + data[i - bpp]);
            }
            break;
        case 2:
            if (prev) {
                for (int i = 0; i < n; ++i) {
                    data[i] = static_cast<uint8_t>(data[i] + prev[i]);
                }
            }
            break;
        case 3:
            for (int i = 0; i < n; ++i) {
                const uint8_t left = (i >= bpp) ? data[i - bpp] : 0;
                const uint8_t up = prev ? prev[i] : 0;
                data[i] = static_cast<uint8_t>(data[i] + ((left + up) / 2));
            }
            break;
        case 4:
            for (int i = 0; i < n; ++i) {
                const uint8_t left = (i >= bpp) ? data[i - bpp] : 0;
                const uint8_t up = prev ? prev[i] : 0;
                const uint8_t up_left = (prev && i >= bpp) ? prev[i - bpp] : 0;
                data[i] = static_cast<uint8_t>(data[i] + Paeth(left, up, up_left));
            }
            break;
        default:
            return false;
    }
    return true;
}

bool PngCancelled(const PngStream& s) {
    return AbortRequested(s.abort) || (s.cancelled != nullptr && s.cancelled(s.cancel_context));
}

void SamplePngRowToL8(const PngStream& s, const uint8_t* row_data, int src_y, RasterImage& out) {
    // 最近邻只选定一条源行，避免高度增加时反复覆盖同一目标行。
    const int dy = static_cast<int>((int64_t(src_y) * out.height + s.height - 1) / s.height);
    if (dy >= out.height || int64_t(dy) * s.height / out.height != src_y) return;
    const int step = s.bit_depth / 8;
    for (int dx = 0; dx < out.width; ++dx) {
        const int sx = int64_t(dx) * s.width / out.width;
        const uint8_t* p = row_data + sx * s.bpp;
        const auto raw = [p, step](int channel) -> unsigned {
            const auto* value = p + channel * step;
            return step == 2 ? (unsigned(value[0]) << 8) | value[1] : value[0];
        };
        const auto sample = [&raw, step](int channel) -> unsigned {
            return step == 2 ? (raw(channel) + 128) / 257 : raw(channel);
        };
        unsigned gray = 128, alpha = 255;
        if (s.color_type == 3) {
            const unsigned index = p[0];
            if (s.pal == nullptr || index >= unsigned(s.pal_n)) continue;
            const uint8_t* color = s.pal + index * 4;
            gray = (color[0] * 30 + color[1] * 59 + color[2] * 11) / 100;
            alpha = color[3];
        } else if (s.color_type == 0 || s.color_type == 4) {
            gray = sample(0);
            if (s.color_type == 0 && s.has_transparent && raw(0) == s.transparent[0]) alpha = 0;
            if (s.color_type == 4) alpha = sample(1);
        } else {
            gray = (sample(0) * 30 + sample(1) * 59 + sample(2) * 11) / 100;
            if (s.color_type == 2 && s.has_transparent && raw(0) == s.transparent[0] &&
                raw(1) == s.transparent[1] && raw(2) == s.transparent[2]) alpha = 0;
            if (s.color_type == 6) alpha = sample(3);
        }
        out.pixels[size_t(dy) * out.width + dx] = (gray * alpha + 255 * (255 - alpha) + 127) / 255;
    }
}

bool PngEmitFullRows(PngStream& s, RasterImage& out) {
    const size_t need = static_cast<size_t>(s.row_bytes) + 1;
    while (s.row_fill >= need && s.row_index < s.height) {
        const uint8_t filter = s.cur[0];
        if (!PngUnfilterRow(s.cur, s.row_index == 0 ? nullptr : s.prev, s.row_bytes, s.bpp, filter)) {
            ESP_LOGW(TAG, "decode=stream png bad filter %u", filter);
            return false;
        }
        SamplePngRowToL8(s, s.cur + 1, static_cast<int>(s.row_index), out);
        std::memcpy(s.prev, s.cur + 1, static_cast<size_t>(s.row_bytes));
        s.row_index++;
        s.row_fill = 0;
    }
    return true;
}

bool PngFeed(PngStream& s, const uint8_t* data, size_t len, bool finish, RasterImage& out) {
    if (!s.z_init) {
        s.z = {};
        s.z.zalloc = PngZalloc;
        s.z.zfree = PngZfree;
        s.z.opaque = s.work;
        if (inflateInit(&s.z) != Z_OK) {
            return false;
        }
        s.z_init = true;
    }

    size_t in_off = 0;
    for (;;) {
        if (PngCancelled(s)) return false;
        if (s.z_end) return in_off == len;
        uint8_t excess;
        const bool rows_done = s.row_index >= s.height;
        const size_t capacity = rows_done ? 1 : (size_t(s.row_bytes) + 1 - s.row_fill);
        const size_t in_left = (data != nullptr && in_off < len) ? (len - in_off) : 0;
        s.z.next_in = (in_left > 0) ? const_cast<Bytef*>(data + in_off) : nullptr;
        s.z.avail_in = static_cast<uInt>(in_left);
        s.z.next_out = rows_done ? &excess : s.cur + s.row_fill;
        s.z.avail_out = static_cast<uInt>(capacity);
        const int zret = inflate(&s.z, finish ? Z_FINISH : Z_NO_FLUSH);
        const size_t took = in_left - s.z.avail_in;
        in_off += took;
        const size_t got = capacity - s.z.avail_out;
        if (rows_done && got != 0) return false;
        s.row_fill += got;

        if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR) {
            ESP_LOGW(TAG, "decode=stream png inflate err=%d", zret);
            return false;
        }
        if (!PngEmitFullRows(s, out)) {
            return false;
        }
        if (zret == Z_STREAM_END) {
            s.z_end = true;
            return s.row_index == s.height && s.row_fill == 0 && in_off == len;
        }
        if ((s.row_index & 31) == 0 && got != 0) vTaskDelay(1);
        if (in_off >= len && (!finish || got == 0)) {
            break;
        }
        if (got == 0 && took == 0) return false;
    }
    return true;
}

using PngReadFn = bool (*)(void* ctx, uint8_t* dst, size_t n);
using PngSkipFn = bool (*)(void* ctx, size_t n);

bool ZipPngRead(void* ctx, uint8_t* dst, size_t n) {
    return StreamReadExact(*static_cast<ZipReader*>(ctx), dst, n);
}

bool ZipPngSkip(void* ctx, size_t n) {
    return StreamSkip(*static_cast<ZipReader*>(ctx), n);
}

struct PngMem {
    const uint8_t* p = nullptr; // PNG 输入数据
    size_t n = 0; // 输入数据总字节数
    size_t off = 0; // 当前读取偏移
};

bool MemPngRead(void* ctx, uint8_t* dst, size_t n) {
    auto* m = static_cast<PngMem*>(ctx);
    if (m->off > m->n || n > m->n - m->off) {
        return false;
    }
    if (n > 0) {
        std::memcpy(dst, m->p + m->off, n);
    }
    m->off += n;
    return true;
}

bool MemPngSkip(void* ctx, size_t n) {
    auto* m = static_cast<PngMem*>(ctx);
    if (m->off > m->n || n > m->n - m->off) {
        return false;
    }
    m->off += n;
    return true;
}

struct PngFile {
    FILE* file; // 当前输入文件
    ImageDecodeCancelled cancelled; // 调用方取消条件
    void* context; // 调用方状态
};
bool FilePngRead(void* context, uint8_t* dst, size_t n) {
    auto* input = static_cast<PngFile*>(context);
    return (input->cancelled == nullptr || !input->cancelled(input->context)) &&
        std::fread(dst, 1, n, input->file) == n;
}
bool FilePngSkip(void* context, size_t n) {
    uint8_t buffer[256];
    while (n != 0) {
        const size_t count = std::min(n, sizeof(buffer));
        if (!FilePngRead(context, buffer, count)) return false;
        n -= count;
    }
    return true;
}

// 按行解压并缩放，仅保留当前行和上一行，避免分配完整 RGBA 图像。
bool DecodePngRows(PngReadFn read, PngSkipFn skip, void* ctx, int max_w, int max_h, RasterImage& out,
                   const std::atomic<bool>* abort, ImageDecodeCancelled cancelled = nullptr,
                   void* cancel_context = nullptr) {
    out.Reset();
    const bool original_size = max_w == 0 && max_h == 0;
    std::vector<uint8_t>().swap(out.pixels);
    PngStream s{};
    s.abort = abort;
    s.cancelled = cancelled;
    s.cancel_context = cancel_context;
    SemaphoreHandle_t mutex = PngMutex();
    if (mutex == nullptr) return false;
    while (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        if (PngCancelled(s)) return false;
    }
    PngLock lock{mutex};
    std::unique_ptr<PngWorkspace, decltype(&heap_caps_free)> work(
        static_cast<PngWorkspace*>(heap_caps_aligned_calloc(16, 1, kPngWorkspaceBytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!work || PngCancelled(s)) return false;
    s.work = work.get();
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> row_buffer(nullptr, heap_caps_free);

    uint8_t sig[8];
    if (!read(ctx, sig, 8) || std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        ESP_LOGW(TAG, "decode=stream png bad signature");
        return false;
    }

    uint8_t* io = work->io;

    const auto skip_bytes = [&](size_t bytes) {
        while (bytes != 0) {
            if (PngCancelled(s)) return false;
            const size_t count = std::min(bytes, kChunkIo);
            if (!skip(ctx, count)) return false;
            bytes -= count;
        }
        return true;
    };
    bool got_ihdr = false;
    bool done = false;
    bool ok = true;

    while (ok && !done) {
        if (PngCancelled(s)) {
            ok = false;
            break;
        }
        uint8_t hdr[8];
        if (!read(ctx, hdr, 8)) {
            ok = false;
            break;
        }
        const uint32_t len = RdBe32(hdr);
        if (len > INT_MAX || (!got_ihdr && std::memcmp(hdr + 4, "IHDR", 4) != 0)) {
            ok = false;
            break;
        }
        const char type[5] = {static_cast<char>(hdr[4]), static_cast<char>(hdr[5]),
                              static_cast<char>(hdr[6]), static_cast<char>(hdr[7]), 0};

        if (std::strcmp(type, "IHDR") == 0) {
            if (got_ihdr || len != 13 || !read(ctx, io, 13) || !skip_bytes(4)) {
                ok = false;
                break;
            }
            s.width = RdBe32(io);
            s.height = RdBe32(io + 4);
            s.bit_depth = io[8];
            s.color_type = io[9];
            const uint8_t interlace = io[12];
            if (interlace != 0 || io[10] != 0 || io[11] != 0) {
                ESP_LOGW(TAG, "decode=stream png interlaced unsupported");
                ok = false;
                break;
            }
            if ((s.bit_depth != 8 && s.bit_depth != 16) || (s.color_type == 3 && s.bit_depth != 8)) {
                ESP_LOGW(TAG, "decode=stream png bit_depth=%u unsupported", s.bit_depth);
                ok = false;
                break;
            }
            switch (s.color_type) {
                case 0:
                    s.bpp = 1;
                    break;
                case 2:
                    s.bpp = 3;
                    break;
                case 3:
                    s.bpp = 1;
                    break;
                case 4:
                    s.bpp = 2;
                    break;
                case 6:
                    s.bpp = 4;
                    break;
                default:
                    ESP_LOGW(TAG, "decode=stream png color_type=%u", s.color_type);
                    ok = false;
                    break;
            }
            if (!ok) {
                break;
            }
            s.bpp *= s.bit_depth / 8;
            const size_t row_bytes = static_cast<size_t>(s.width) * static_cast<size_t>(s.bpp);
            if (s.width == 0 || s.width > UINT16_MAX || s.height == 0 || s.height > INT_MAX) {
                ESP_LOGW(TAG, "decode=stream png size reject %ux%u row=%u",
                         static_cast<unsigned>(s.width), static_cast<unsigned>(s.height),
                         static_cast<unsigned>(row_bytes));
                ok = false;
                break;
            }
            s.row_bytes = static_cast<int>(row_bytes);
            row_buffer.reset(static_cast<uint8_t*>(heap_caps_malloc(row_bytes * 2 + 1,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
            if (!row_buffer) { ok = false; break; }
            s.prev = row_buffer.get();
            s.cur = row_buffer.get() + row_bytes;
            std::memset(s.prev, 0, row_bytes);

            int dst_w = 0;
            int dst_h = 0;
            FitImageSize(static_cast<int>(s.width), static_cast<int>(s.height),
                    original_size ? static_cast<int>(s.width) : max_w,
                    original_size ? static_cast<int>(s.height) : max_h, &dst_w,
                    &dst_h);
            const size_t output_bytes = size_t(dst_w) * dst_h;
            if (dst_w <= 0 || dst_h <= 0 || dst_w > UINT16_MAX || dst_h > UINT16_MAX ||
                (!original_size && output_bytes > kMaxPngOutput) ||
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < output_bytes ||
                !AllocRasterL8(out, dst_w, dst_h) ||
                (output_bytes > 4096 && !esp_ptr_external_ram(out.pixels.data()))) {
                ESP_LOGW(TAG, "decode=stream png output reject %dx%d bytes=%u", dst_w, dst_h, unsigned(output_bytes));
                ok = false;
                break;
            }
            got_ihdr = true;
            ESP_LOGI(TAG, "decode=stream png %ux%u type=%u depth=%u work=%u -> %dx%d", unsigned(s.width),
                     unsigned(s.height), s.color_type, s.bit_depth, unsigned(kPngWorkspaceBytes), dst_w, dst_h);
        } else if (std::strcmp(type, "PLTE") == 0) {
            if (s.z_init || len == 0 || (len % 3) != 0 || len > 256 * 3 || len > kChunkIo || !read(ctx, io, len) ||
                !skip_bytes(4)) {
                ok = false;
                break;
            }
            if (s.pal == nullptr) {
                s.pal = work->palette;
            }
            s.pal_n = static_cast<int>(len / 3);
            for (int i = 0; i < s.pal_n; ++i) {
                s.pal[i * 4 + 0] = io[i * 3 + 0];
                s.pal[i * 4 + 1] = io[i * 3 + 1];
                s.pal[i * 4 + 2] = io[i * 3 + 2];
                s.pal[i * 4 + 3] = 255;
            }
        } else if (std::strcmp(type, "tRNS") == 0) {
            if (s.z_init) { ok = false; break; }
            if (s.color_type == 0 || s.color_type == 2) {
                const unsigned channels = s.color_type == 0 ? 1 : 3;
                if (len != channels * 2 || !read(ctx, io, len) || !skip_bytes(4)) {
                    ok = false;
                    break;
                }
                for (unsigned i = 0; i < channels; ++i) {
                    s.transparent[i] = static_cast<uint16_t>((static_cast<unsigned>(io[i * 2]) << 8) | io[i * 2 + 1]);
                }
                s.has_transparent = true;
            } else if (s.color_type == 3 && s.pal != nullptr && len > 0 &&
                len <= static_cast<uint32_t>(s.pal_n) && len <= kChunkIo) {
                if (!read(ctx, io, len) || !skip_bytes(4)) {
                    ok = false;
                    break;
                }
                for (uint32_t i = 0; i < len; ++i) {
                    s.pal[i * 4 + 3] = io[i];
                }
            } else if (!skip_bytes(static_cast<size_t>(len) + 4)) {
                ok = false;
                break;
            }
        } else if (std::strcmp(type, "IDAT") == 0) {
            if (!got_ihdr || (s.color_type == 3 && s.pal_n <= 0)) {
                ok = false;
                break;
            }
            uint32_t left = len;
            while (left > 0) {
                if (PngCancelled(s)) {
                    ok = false;
                    break;
                }
                const size_t n = left > kChunkIo ? kChunkIo : left;
                if (!read(ctx, io, n)) {
                    ok = false;
                    break;
                }
                if (!PngFeed(s, io, n, false, out)) {
                    ok = false;
                    break;
                }
                left -= static_cast<uint32_t>(n);
            }
            if (!ok || !skip_bytes(4)) {
                ok = false;
                break;
            }
        } else if (std::strcmp(type, "IEND") == 0) {
            if (len != 0 || !skip_bytes(4)) {
                ok = false;
                break;
            }
            if (got_ihdr && !s.z_end) {
                ok = PngFeed(s, nullptr, 0, true, out);
            }
            done = true;
        } else if (!skip_bytes(static_cast<size_t>(len) + 4)) {
            ok = false;
            break;
        }
    }

    const uint32_t rows = s.row_index;
    const uint32_t h = s.height;
    PngStreamFree(s);

    if (!ok || !done || !got_ihdr || !s.z_end || PngCancelled(s) || out.empty() || rows != h) {
        if (ok && got_ihdr && rows < h) {
            ESP_LOGW(TAG, "decode=stream png incomplete rows %u/%u", static_cast<unsigned>(rows),
                     static_cast<unsigned>(h));
        }
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
        return false;
    }
    out.BindDsc();
    return true;
}

bool DecodePngStream(ZipReader& zip, int max_w, int max_h, RasterImage& out,
                     const std::atomic<bool>* abort) {
    return DecodePngRows(ZipPngRead, ZipPngSkip, &zip, max_w, max_h, out, abort);
}

}  // namespace

bool DecodePngMemoryToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out,
                         ImageDecodeCancelled cancelled, void* context) {
    if (data == nullptr || len < 8 || ((max_w != 0 || max_h != 0) && (max_w <= 0 || max_h <= 0))) {
        out.Reset();
        return false;
    }
    PngMem mem{data, len, 0};
    return DecodePngRows(MemPngRead, MemPngSkip, &mem, max_w, max_h, out, nullptr, cancelled, context);
}

bool DecodePngFileToL8(const char* path, int max_w, int max_h, RasterImage& out,
                       ImageDecodeCancelled cancelled, void* context) {
    out.Reset();
    if (path == nullptr || ((max_w != 0 || max_h != 0) && (max_w <= 0 || max_h <= 0)) ||
        (cancelled && cancelled(context))) return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    PngFile input{file, cancelled, context};
    const bool ok = DecodePngRows(FilePngRead, FilePngSkip, &input, max_w, max_h, out,
                                  nullptr, cancelled, context);
    std::fclose(file);
    if (!ok && (cancelled == nullptr || !cancelled(context))) ESP_LOGW(TAG, "png decode failed: %s", path);
    return ok;
}

bool DecodeZipEntryImageToL8(ZipReader& zip, const char* entry_name, int max_w, int max_h,
                             RasterImage& out, const std::atomic<bool>* abort) {
    out.Reset();
    if (entry_name == nullptr || max_w <= 0 || max_h <= 0) {
        return false;
    }
    if (AbortRequested(abort)) {
        return false;
    }
    if (!zip.OpenEntryStream(entry_name)) {
        return false;
    }

    uint8_t head[8];
    if (!StreamReadExact(zip, head, 8)) {
        zip.CloseEntryStream();
        return false;
    }
    zip.CloseEntryStream();
    if (AbortRequested(abort) || !zip.OpenEntryStream(entry_name)) {
        return false;
    }

    bool ok = false;
    if (head[0] == 0xFF && head[1] == 0xD8) {
#if READER_HAS_TJPGD
        ok = DecodeJpegStream(zip, max_w, max_h, out, abort);
        if (!ok && !AbortRequested(abort)) {
            zip.CloseEntryStream();
            if (zip.OpenEntryStream(entry_name)) ok = DecodeImageBuffered(zip, max_w, max_h, out, abort);
        }
#else
        ok = DecodeImageBuffered(zip, max_w, max_h, out, abort);
#endif
    } else if (std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) {
        ok = DecodePngStream(zip, max_w, max_h, out, abort);
    } else {
        ESP_LOGW(TAG, "decode=stream unknown format entry=%s", entry_name);
    }

    if (AbortRequested(abort)) {
        ESP_LOGI(TAG, "decode=stream abort entry=%s", entry_name);
        ok = false;
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
    } else if (zip.EntryStreamError()) {
        ESP_LOGW(TAG, "decode=stream zip error entry=%s", entry_name);
        ok = false;
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
    }
    zip.CloseEntryStream();
    if (ok) {
        ESP_LOGI(TAG, "decode=stream ok entry=%s out=%ux%u", entry_name, out.width, out.height);
    }
    return ok;
}

}  // namespace reader
