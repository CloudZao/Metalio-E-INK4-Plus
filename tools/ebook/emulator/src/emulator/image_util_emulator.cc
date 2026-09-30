/**
 * image_util 模拟器版：lodepng + 简易 JPEG（tjpgd）。
 */
#include "image_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define LODEPNG_NO_COMPILE_CPP
#include "../../lvgl/src/libs/lodepng/lodepng.h"

#if LV_USE_TJPGD
#include "../../lvgl/src/libs/tjpgd/tjpgd.h"
#endif

namespace reader {
namespace {

constexpr const char* TAG = "ReaderImg";

constexpr uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

uint8_t GrayToL8Dither(uint8_t gray, int x, int y) {
    const int level = (255 - gray) * 16 / 256;
    const int thr = kBayer4[y & 3][x & 3];
    return (level > thr) ? 0x00 : 0xFF;
}

bool LooksLikeJpeg(const uint8_t* data, size_t len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool LooksLikePng(const uint8_t* data, size_t len) {
    static const uint8_t kSig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= 8 && std::memcmp(data, kSig, 8) == 0;
}

bool LooksLikeA2i1(const uint8_t* data, size_t len) {
    return len >= 20 && data[0] == 'A' && data[1] == '2' && data[2] == 'I' && data[3] == '1';
}

bool LooksLikeEbgr(const uint8_t* data, size_t len) {
    return len >= 8 && data[0] == 'E' && data[1] == 'B' && data[2] == 'G' && data[3] == 'R';
}

bool DecodeA2i1ToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    if (!LooksLikeA2i1(data, len)) {
        return false;
    }
    const uint16_t src_w = static_cast<uint16_t>(data[4] | (data[5] << 8));
    const uint16_t src_h = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint16_t stride = static_cast<uint16_t>(data[8] | (data[9] << 8));
    if (src_w == 0 || src_h == 0 || stride == 0) {
        return false;
    }
    const size_t need = 20u + static_cast<size_t>(stride) * src_h;
    if (len < need) {
        return false;
    }
    const uint8_t* bits = data + 20;
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        const uint8_t* row = bits + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            const uint8_t byte = row[sx >> 3];
            const bool white = (byte & (0x80 >> (sx & 7))) != 0;
            out.pixels[static_cast<size_t>(y) * dst_w + x] = white ? 0xFF : 0x00;
        }
    }
    out.BindDsc();
    return true;
}

bool DecodeEbgrToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    if (!LooksLikeEbgr(data, len)) {
        return false;
    }
    const int src_w = data[4] | (data[5] << 8);
    const int src_h = data[6] | (data[7] << 8);
    if (src_w <= 0 || src_h <= 0) {
        return false;
    }
    const size_t need = 8u + static_cast<size_t>(src_w) * src_h;
    if (len < need) {
        return false;
    }
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);
    const uint8_t* gray = data + 8;
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            const uint8_t g = gray[static_cast<size_t>(sy) * src_w + sx];
            out.pixels[static_cast<size_t>(y) * dst_w + x] = GrayToL8Dither(g, x, y);
        }
    }
    out.BindDsc();
    return true;
}

void Rgb888ToL8Scaled(const uint8_t* rgba, int src_w, int src_h, bool has_alpha, int max_w, int max_h,
                      RasterImage& out) {
    if (src_w <= 0 || src_h <= 0) {
        out.Reset();
        return;
    }
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    out.pixels.assign(static_cast<size_t>(dst_w) * dst_h, 0xFF);
    out.width = static_cast<uint16_t>(dst_w);
    out.height = static_cast<uint16_t>(dst_h);
    const int bpp = has_alpha ? 4 : 3;
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            const uint8_t* p = rgba + (static_cast<size_t>(sy) * src_w + sx) * bpp;
            uint8_t r = p[0], g = p[1], b = p[2];
            if (has_alpha && p[3] < 128) {
                r = g = b = 255;
            }
            const uint8_t gray = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            out.pixels[static_cast<size_t>(y) * dst_w + x] = GrayToL8Dither(gray, x, y);
        }
    }
    out.BindDsc();
}

#if LV_USE_TJPGD
struct JpgOutCtx {
    std::vector<uint8_t>* rgb;
    int w = 0;
};

static int jpg_out_func(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* ctx = static_cast<JpgOutCtx*>(jd->device);
    uint8_t* src = static_cast<uint8_t*>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        for (int x = rect->left; x <= rect->right; ++x) {
            const size_t i = (static_cast<size_t>(y) * ctx->w + x) * 3;
            ctx->rgb->data()[i] = src[0];
            ctx->rgb->data()[i + 1] = src[1];
            ctx->rgb->data()[i + 2] = src[2];
            src += 3;
        }
    }
    return 1;
}

static size_t jpg_in_func(JDEC* jd, uint8_t* buff, size_t ndata) {
    struct JpgInCtx {
        const uint8_t* data;
        size_t len;
        size_t pos;
    };
    auto* ctx = static_cast<JpgInCtx*>(jd->device);
    if (buff == nullptr) {
        return 0;
    }
    const size_t remain = ctx->len - ctx->pos;
    const size_t n = ndata < remain ? ndata : remain;
    if (n > 0) {
        memcpy(buff, ctx->data + ctx->pos, n);
        ctx->pos += n;
    }
    return n;
}

bool DecodeJpegToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    struct JpgInCtx {
        const uint8_t* data;
        size_t len;
        size_t pos = 0;
    } in{data, len};

    JDEC jd{};
    uint8_t pool[4096];
    JRESULT r = jd_prepare(&jd, [](JDEC* j, uint8_t* b, size_t n) -> size_t {
        auto* ctx = static_cast<JpgInCtx*>(j->device);
        if (b == nullptr) {
            return 0;
        }
        const size_t remain = ctx->len - ctx->pos;
        const size_t take = n < remain ? n : remain;
        if (take > 0) {
            memcpy(b, ctx->data + ctx->pos, take);
            ctx->pos += take;
        }
        return take;
    }, pool, sizeof(pool), &in);
    if (r != JDR_OK) {
        return false;
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(jd.width) * jd.height * 3);
    JpgOutCtx out_ctx{&rgb, static_cast<int>(jd.width)};
    jd.device = &out_ctx;
    r = jd_decomp(&jd, jpg_out_func, 0);
    if (r != JDR_OK) {
        return false;
    }
    Rgb888ToL8Scaled(rgb.data(), static_cast<int>(jd.width), static_cast<int>(jd.height), false, max_w,
                     max_h, out);
    return !out.empty();
}
#endif

}  // namespace

bool DecodeImageToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (data == nullptr || len == 0 || max_w <= 0 || max_h <= 0) {
        return false;
    }
    if (LooksLikeA2i1(data, len)) {
        return DecodeA2i1ToL8(data, len, max_w, max_h, out);
    }
    if (LooksLikeEbgr(data, len)) {
        return DecodeEbgrToL8(data, len, max_w, max_h, out);
    }
    if (LooksLikePng(data, len)) {
        unsigned char* rgba = nullptr;
        unsigned w = 0, h = 0;
        const unsigned err = lodepng_decode32(&rgba, &w, &h, data, len);
        if (err != 0 || rgba == nullptr) {
            if (rgba) {
                free(rgba);
            }
            return false;
        }
        Rgb888ToL8Scaled(rgba, static_cast<int>(w), static_cast<int>(h), true, max_w, max_h, out);
        free(rgba);
        return !out.empty();
    }
    if (LooksLikeJpeg(data, len)) {
#if LV_USE_TJPGD
        return DecodeJpegToL8(data, len, max_w, max_h, out);
#else
        ESP_LOGW(TAG, "jpeg unsupported (TJPGD off)");
        return false;
#endif
    }
    return false;
}

bool DecodeImageFileToL8(const char* path, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (path == nullptr) {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const long sz = std::ftell(fp);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        std::fclose(fp);
        return false;
    }
    std::rewind(fp);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);
    return DecodeImageToL8(buf.data(), buf.size(), max_w, max_h, out);
}

}  // namespace reader
