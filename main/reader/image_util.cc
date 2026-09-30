#include "image_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include "jpg/jpeg_to_image.h"
#endif

namespace reader {
namespace {

constexpr const char* TAG = "ReaderImg";

constexpr uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

uint8_t GrayToL8Dither(uint8_t gray, int x, int y) {
    // gray 0=黑 255=白；阈值映射 0..15，低于阈值为黑
    const int level = (255 - gray) * 16 / 256;  // 黑度
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
    // A2I1: magic + w/h/stride/res(2) + 8B palette + bitmap (1=白 0=黑, MSB left)
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
                r = g = b = 255;  // 透明当白
            }
            const uint8_t gray = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            out.pixels[static_cast<size_t>(y) * dst_w + x] = GrayToL8Dither(gray, x, y);
        }
    }
    out.BindDsc();
}

void Rgb565ToL8Scaled(const uint8_t* rgb565, int src_w, int src_h, size_t stride, int max_w, int max_h,
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

    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        const uint8_t* row = rgb565 + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            const uint16_t pix = static_cast<uint16_t>(row[sx * 2] | (row[sx * 2 + 1] << 8));
            const uint8_t r = static_cast<uint8_t>(((pix >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((pix >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((pix & 0x1F) * 255 / 31);
            const uint8_t gray = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            out.pixels[static_cast<size_t>(y) * dst_w + x] = GrayToL8Dither(gray, x, y);
        }
    }
    out.BindDsc();
}

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
            ESP_LOGW(TAG, "png decode err %u", err);
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
#ifndef CONFIG_IDF_TARGET_ESP32
        uint8_t* rgb565 = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        // 解码阶段按 max 缩小（最多约 1/8）+ PSRAM，避免大封面全尺寸 RGB565 OOM
        if (jpeg_to_image_fit(data, len, max_w, max_h, &rgb565, &out_len, &w, &h, &stride) != ESP_OK ||
            rgb565 == nullptr) {
            ESP_LOGW(TAG, "jpeg decode failed");
            return false;
        }
        Rgb565ToL8Scaled(rgb565, static_cast<int>(w), static_cast<int>(h), stride, max_w, max_h, out);
        heap_caps_free(rgb565);
        return !out.empty();
#else
        ESP_LOGW(TAG, "jpeg not supported on esp32");
        return false;
#endif
    }

    ESP_LOGW(TAG, "unknown image format (%u bytes)", static_cast<unsigned>(len));
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

bool PeekA2i1Size(const uint8_t* data, size_t len, int* w, int* h) {
    if (w == nullptr || h == nullptr || !LooksLikeA2i1(data, len) || len < 10) {
        return false;
    }
    const int sw = data[4] | (data[5] << 8);
    const int sh = data[6] | (data[7] << 8);
    if (sw <= 0 || sh <= 0) {
        return false;
    }
    *w = sw;
    *h = sh;
    return true;
}

bool PeekA2i1FileSize(const char* path, int* w, int* h) {
    if (path == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    uint8_t hdr[20];
    const size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
    std::fclose(fp);
    return PeekA2i1Size(hdr, n, w, h);
}

bool ScaleRasterToFit(const RasterImage& src, int max_w, int max_h, RasterImage& out) {
    if (src.empty() || max_w <= 0 || max_h <= 0) {
        out.Reset();
        return false;
    }
    int dst_w = src.width;
    int dst_h = src.height;
    if (dst_w > max_w || dst_h > max_h) {
        const float sx = static_cast<float>(max_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(max_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
    }
    if (dst_w == src.width && dst_h == src.height) {
        if (&out != &src) {
            out = src;
        }
        out.BindDsc();
        return true;
    }

    RasterImage tmp;
    tmp.pixels.assign(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h), 0xFF);
    tmp.width = static_cast<uint16_t>(dst_w);
    tmp.height = static_cast<uint16_t>(dst_h);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src.height / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src.width / dst_w;
            tmp.pixels[static_cast<size_t>(y) * dst_w + x] =
                src.pixels[static_cast<size_t>(sy) * src.width + sx];
        }
    }
    out = std::move(tmp);
    out.BindDsc();
    return !out.empty();
}

bool ValidateA2i1Bytes(const uint8_t* data, size_t len) {
    if (!LooksLikeA2i1(data, len)) {
        return false;
    }
    const uint16_t src_w = static_cast<uint16_t>(data[4] | (data[5] << 8));
    const uint16_t src_h = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint16_t stride = static_cast<uint16_t>(data[8] | (data[9] << 8));
    if (src_w == 0 || src_h == 0 || stride == 0) {
        return false;
    }
    const size_t min_stride = (static_cast<size_t>(src_w) + 7u) / 8u;
    if (stride < min_stride) {
        return false;
    }
    const size_t need = 20u + static_cast<size_t>(stride) * static_cast<size_t>(src_h);
    return len >= need;
}

bool EncodeL8ToA2i1(const RasterImage& img, std::vector<uint8_t>& out) {
    out.clear();
    if (img.empty() || img.width == 0 || img.height == 0) {
        return false;
    }
    const int w = img.width;
    const int h = img.height;
    const int stride = (w + 7) / 8;
    const size_t bitmap_bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    out.assign(20 + bitmap_bytes, 0);
    out[0] = 'A';
    out[1] = '2';
    out[2] = 'I';
    out[3] = '1';
    out[4] = static_cast<uint8_t>(w & 0xFF);
    out[5] = static_cast<uint8_t>((w >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>(h & 0xFF);
    out[7] = static_cast<uint8_t>((h >> 8) & 0xFF);
    out[8] = static_cast<uint8_t>(stride & 0xFF);
    out[9] = static_cast<uint8_t>((stride >> 8) & 0xFF);
    // LVGL I1 palette：index0 黑、index1 白（ARGB8888）
    out[12] = 0xFF;
    out[13] = 0x00;
    out[14] = 0x00;
    out[15] = 0x00;
    out[16] = 0xFF;
    out[17] = 0xFF;
    out[18] = 0xFF;
    out[19] = 0xFF;

    uint8_t* bits = out.data() + 20;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            if (row[x] >= 0x80) {
                bits[static_cast<size_t>(y) * static_cast<size_t>(stride) + (x >> 3)] |=
                    static_cast<uint8_t>(0x80 >> (x & 7));
            }
        }
    }
    return true;
}

bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len) {
    if (path == nullptr || path[0] == '\0' || data == nullptr || len == 0) {
        return false;
    }
    std::string tmp(path);
    tmp.append(".tmp");
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(data, 1, len, f) == len;
    if (ok && std::fflush(f) != 0) {
        ok = false;
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    // FatFS：目标存在时 rename 常失败 → 先删旧。若随后 rename 仍失败，保留 .tmp
    // 供调用方/LoadPos 回退；切勿再 unlink(tmp)，否则唯一完好副本被毁掉。
    unlink(path);
    if (rename(tmp.c_str(), path) != 0) {
        return false;
    }
    return true;
}

}  // namespace reader
