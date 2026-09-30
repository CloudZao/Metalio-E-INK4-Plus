#include "image_stream.h"

// xtensa-g++：本文件在 -Os 下可能 ICE（cfgcleanup try_forward_edges）
#pragma GCC optimize("O1")

#include <algorithm>
#include <atomic>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/tjpgd.h"
#define READER_HAS_TJPGD 1
#elif CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/tjpgd.h"
#define READER_HAS_TJPGD 1
#else
#define READER_HAS_TJPGD 0
#endif

#include <zlib.h>

namespace reader {
namespace {

constexpr const char* TAG = "ImgStream";
constexpr size_t kChunkIo = 8 * 1024;
constexpr size_t kMaxPngRowBytes = 256 * 1024;

constexpr uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

uint8_t GrayToL8Dither(uint8_t gray, int x, int y) {
    const int level = (255 - gray) * 16 / 256;
    const int thr = kBayer4[y & 3][x & 3];
    return (level > thr) ? 0x00 : 0xFF;
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
    out.pixels.assign(n, 0xFF);
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

void FitSize(int src_w, int src_h, int max_w, int max_h, int* dst_w, int* dst_h) {
    int w = src_w;
    int h = src_h;
    if (w > max_w || h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(h);
        const float s = sx < sy ? sx : sy;
        w = std::max(1, static_cast<int>(w * s));
        h = std::max(1, static_cast<int>(h * s));
    }
    *dst_w = w;
    *dst_h = h;
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
            ctx->out->pixels[static_cast<size_t>(dy) * ctx->out_w + dx] = GrayToL8Dither(gray, dx, dy);
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
    FitSize(ctx.src_w, ctx.src_h, max_w, max_h, &fit_w, &fit_h);
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
    FitSize(ctx.dec_w, ctx.dec_h, max_w, max_h, &ctx.out_w, &ctx.out_h);
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

#else

bool DecodeJpegStream(ZipReader& /*zip*/, int /*max_w*/, int /*max_h*/, RasterImage& out,
                      const std::atomic<bool>* /*abort*/) {
    out.Reset();
    ESP_LOGW(TAG, "decode=stream jpeg unsupported on this target");
    return false;
}

#endif

struct PngStream {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t color_type = 0;
    int bpp = 0;
    int row_bytes = 0;

    z_stream z{};
    bool z_init = false;
    uint8_t* prev = nullptr;
    uint8_t* cur = nullptr;
    uint32_t row_index = 0;
    size_t row_fill = 0;
};

void PngStreamFree(PngStream& s) {
    if (s.z_init) {
        inflateEnd(&s.z);
        s.z_init = false;
    }
    heap_caps_free(s.prev);
    heap_caps_free(s.cur);
    s.prev = s.cur = nullptr;
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

void SamplePngRowToL8(const uint8_t* row_data, int src_w, int src_y, int bpp, int color_type,
                      int src_h, RasterImage& out) {
    const int dy = src_y * out.height / src_h;
    if (dy < 0 || dy >= out.height) {
        return;
    }
    for (int dx = 0; dx < out.width; ++dx) {
        const int sx = dx * src_w / out.width;
        const uint8_t* p = row_data + sx * bpp;
        uint8_t gray = 128;
        if (color_type == 0 || color_type == 4) {
            gray = p[0];
        } else if (color_type == 2 || color_type == 6) {
            gray = static_cast<uint8_t>((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100);
        } else if (color_type == 3) {
            gray = p[0];
        }
        out.pixels[static_cast<size_t>(dy) * out.width + dx] = GrayToL8Dither(gray, dx, dy);
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
        SamplePngRowToL8(s.cur + 1, static_cast<int>(s.width), static_cast<int>(s.row_index), s.bpp,
                         s.color_type, static_cast<int>(s.height), out);
        std::memcpy(s.prev, s.cur + 1, static_cast<size_t>(s.row_bytes));
        s.row_index++;
        s.row_fill = 0;
    }
    return true;
}

bool PngFeed(PngStream& s, const uint8_t* data, size_t len, bool finish, RasterImage& out) {
    if (!s.z_init) {
        s.z = {};
        if (inflateInit(&s.z) != Z_OK) {
            return false;
        }
        s.z_init = true;
    }

    size_t in_off = 0;
    for (;;) {
        if (s.row_index >= s.height) {
            return true;
        }
        const size_t in_left = (data != nullptr && in_off < len) ? (len - in_off) : 0;
        s.z.next_in = (in_left > 0) ? const_cast<Bytef*>(data + in_off) : nullptr;
        s.z.avail_in = static_cast<uInt>(in_left);
        s.z.next_out = s.cur + s.row_fill;
        s.z.avail_out = static_cast<uInt>((static_cast<size_t>(s.row_bytes) + 1) - s.row_fill);
        const int zret = inflate(&s.z, finish ? Z_FINISH : Z_NO_FLUSH);
        const size_t took = in_left - s.z.avail_in;
        in_off += took;
        const size_t got = ((static_cast<size_t>(s.row_bytes) + 1) - s.row_fill) - s.z.avail_out;
        s.row_fill += got;

        if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR) {
            ESP_LOGW(TAG, "decode=stream png inflate err=%d", zret);
            return false;
        }
        if (!PngEmitFullRows(s, out)) {
            return false;
        }
        if (s.row_index >= s.height) {
            return true;
        }
        if (zret == Z_STREAM_END) {
            return s.row_index >= s.height;
        }
        if (in_off >= len && (!finish || got == 0)) {
            break;
        }
        if (finish && got == 0 && took == 0) {
            break;
        }
    }
    return true;
}

bool DecodePngStream(ZipReader& zip, int max_w, int max_h, RasterImage& out,
                     const std::atomic<bool>* abort) {
    out.Reset();
    PngStream s{};

    uint8_t sig[8];
    if (!StreamReadExact(zip, sig, 8) || std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        ESP_LOGW(TAG, "decode=stream png bad signature");
        return false;
    }

    uint8_t* io = AllocPs(kChunkIo);
    if (io == nullptr) {
        ESP_LOGE(TAG, "decode=stream png io alloc fail");
        return false;
    }

    bool got_ihdr = false;
    bool done = false;
    bool ok = true;

    while (ok && !done && !zip.EntryStreamError()) {
        if (AbortRequested(abort)) {
            ok = false;
            break;
        }
        uint8_t hdr[8];
        if (!StreamReadExact(zip, hdr, 8)) {
            ok = false;
            break;
        }
        const uint32_t len = RdBe32(hdr);
        const char type[5] = {static_cast<char>(hdr[4]), static_cast<char>(hdr[5]),
                              static_cast<char>(hdr[6]), static_cast<char>(hdr[7]), 0};

        if (std::strcmp(type, "IHDR") == 0) {
            if (len != 13 || !StreamReadExact(zip, io, 13) || !StreamSkip(zip, 4)) {
                ok = false;
                break;
            }
            s.width = RdBe32(io);
            s.height = RdBe32(io + 4);
            const uint8_t bit_depth = io[8];
            s.color_type = io[9];
            const uint8_t interlace = io[12];
            if (interlace != 0) {
                ESP_LOGW(TAG, "decode=stream png interlaced unsupported");
                ok = false;
                break;
            }
            if (bit_depth != 8) {
                ESP_LOGW(TAG, "decode=stream png bit_depth=%u unsupported", bit_depth);
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
            const size_t row_bytes = static_cast<size_t>(s.width) * static_cast<size_t>(s.bpp);
            if (s.width == 0 || s.height == 0 || row_bytes > kMaxPngRowBytes) {
                ESP_LOGW(TAG, "decode=stream png size reject %ux%u row=%u",
                         static_cast<unsigned>(s.width), static_cast<unsigned>(s.height),
                         static_cast<unsigned>(row_bytes));
                ok = false;
                break;
            }
            s.row_bytes = static_cast<int>(row_bytes);
            s.prev = AllocPs(row_bytes);
            s.cur = AllocPs(row_bytes + 1);
            if (s.prev == nullptr || s.cur == nullptr) {
                ok = false;
                break;
            }
            std::memset(s.prev, 0, row_bytes);

            int dst_w = 0;
            int dst_h = 0;
            FitSize(static_cast<int>(s.width), static_cast<int>(s.height), max_w, max_h, &dst_w,
                    &dst_h);
            if (!AllocRasterL8(out, dst_w, dst_h)) {
                ok = false;
                break;
            }
            got_ihdr = true;
            ESP_LOGI(TAG, "decode=stream png %ux%u type=%u -> %dx%d", static_cast<unsigned>(s.width),
                     static_cast<unsigned>(s.height), s.color_type, dst_w, dst_h);
        } else if (std::strcmp(type, "IDAT") == 0) {
            if (!got_ihdr) {
                ok = false;
                break;
            }
            uint32_t left = len;
            while (left > 0) {
                if (AbortRequested(abort)) {
                    ok = false;
                    break;
                }
                const size_t n = left > kChunkIo ? kChunkIo : left;
                if (!StreamReadExact(zip, io, n)) {
                    ok = false;
                    break;
                }
                if (!PngFeed(s, io, n, false, out)) {
                    ok = false;
                    break;
                }
                left -= static_cast<uint32_t>(n);
                if (s.row_index >= s.height) {
                    // 余下 IDAT / CRC 仍要读完，避免污染后续（本条目用完即关）
                    if (left > 0 && !StreamSkip(zip, left)) {
                        ok = false;
                    }
                    left = 0;
                    break;
                }
            }
            if (!ok || !StreamSkip(zip, 4)) {
                ok = false;
                break;
            }
        } else if (std::strcmp(type, "IEND") == 0) {
            if (len > 0 && !StreamSkip(zip, len)) {
                ok = false;
                break;
            }
            StreamSkip(zip, 4);
            if (got_ihdr && s.row_index < s.height) {
                ok = PngFeed(s, nullptr, 0, true, out);
            }
            done = true;
        } else {
            // PLTE/tRNS 等：丢弃
            if (!StreamSkip(zip, static_cast<size_t>(len) + 4)) {
                ok = false;
                break;
            }
        }
    }

    const uint32_t rows = s.row_index;
    const uint32_t h = s.height;
    PngStreamFree(s);
    heap_caps_free(io);

    if (!ok || !got_ihdr || out.empty() || rows < h) {
        if (ok && got_ihdr && rows < h) {
            ESP_LOGW(TAG, "decode=stream png incomplete rows %u/%u", static_cast<unsigned>(rows),
                     static_cast<unsigned>(h));
        }
        out.Reset();
        return false;
    }
    out.BindDsc();
    return true;
}

}  // namespace

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
        ok = DecodeJpegStream(zip, max_w, max_h, out, abort);
    } else if (std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) {
        ok = DecodePngStream(zip, max_w, max_h, out, abort);
    } else {
        ESP_LOGW(TAG, "decode=stream unknown format entry=%s", entry_name);
    }

    if (AbortRequested(abort)) {
        ESP_LOGI(TAG, "decode=stream abort entry=%s", entry_name);
        ok = false;
        out.Reset();
    } else if (zip.EntryStreamError()) {
        ESP_LOGW(TAG, "decode=stream zip error entry=%s", entry_name);
        ok = false;
        out.Reset();
    }
    zip.CloseEntryStream();
    if (ok) {
        ESP_LOGI(TAG, "decode=stream ok entry=%s out=%ux%u", entry_name, out.width, out.height);
    }
    return ok;
}

}  // namespace reader
