#include "image_util.h"
#include "image_stream.h"
#include "image_geometry.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace reader {

bool DecodeImageFileOverlay(const char* path, int max_w, int max_h, RasterImage& out,
                           std::vector<uint8_t>& alpha) try {
    out.Reset();
    alpha.clear();
    if (!path || max_w <= 0 || max_h <= 0) return false;
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), std::fclose);
    if (!file) return false;
    uint8_t header[20] = {};
    const size_t count = std::fread(header, 1, sizeof(header), file.get());
    if (count >= 8 && std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0) {
        file.reset();
        return DecodePngFileToL8(path, max_w, max_h, out, nullptr, nullptr, &alpha);
    }
    const bool i1 = count == 20 && std::memcmp(header, "A2I1", 4) == 0;
    const bool l8 = count >= 8 && std::memcmp(header, "A2L8", 4) == 0;
    if (i1 || l8) {
        const int width = header[4] | (unsigned(header[5]) << 8);
        const int height = header[6] | (unsigned(header[7]) << 8);
        const int stride = i1 ? header[8] | (unsigned(header[9]) << 8) : width;
        const long offset = i1 ? 20 : 8;
        if (width <= 0 || height <= 0 || stride < (i1 ? (width + 7) / 8 : width) ||
            std::fseek(file.get(), 0, SEEK_END) != 0) return false;
        const long length = std::ftell(file.get());
        if (length < 0 || uint64_t(length) < uint64_t(offset) + uint64_t(stride) * height) return false;
        int fitted_w = 0, fitted_h = 0;
        if (!FitImageSize(width, height, max_w, max_h, &fitted_w, &fitted_h)) return false;
        out.width = fitted_w;
        out.height = fitted_h;
        out.pixels.resize(size_t(out.width) * out.height);
        if (i1 && (header[12] != 255 || header[16] != 255)) alpha.resize(out.pixels.size());
        const int bytes = i1 ? (width + 7) / 8 : width;
        std::vector<uint8_t> row(bytes);
        for (int y = 0; y < out.height; ++y) {
            const int sy = int64_t(y) * height / out.height;
            if (std::fseek(file.get(), offset + long(sy) * stride, SEEK_SET) != 0 ||
                std::fread(row.data(), 1, row.size(), file.get()) != row.size()) return false;
            for (int x = 0; x < out.width; ++x) {
                const size_t index = size_t(y) * out.width + x;
                const int sx = int64_t(x) * width / out.width;
                if (!i1) {
                    out.pixels[index] = row[sx];
                    continue;
                }
                const unsigned color = (row[sx / 8] >> (7 - sx % 8)) & 1;
                const uint8_t* palette = header + 12 + color * 4;
                out.pixels[index] = (palette[1] * 30 + palette[2] * 59 + palette[3] * 11) / 100;
                if (!alpha.empty()) alpha[index] = palette[0];
            }
        }
        out.BindDsc();
        return true;
    }
    file.reset();
    return DecodeImageFileToL8(path, 0, 0, out) && ScaleRasterToFit(out, max_w, max_h, out);
} catch (const std::bad_alloc&) {
    out.Reset();
    alpha.clear();
    return false;
}

} // namespace reader
