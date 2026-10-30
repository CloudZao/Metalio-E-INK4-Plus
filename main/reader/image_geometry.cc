#include "image_geometry.h"

#include <algorithm>
#include <cstring>

namespace reader {

bool FitImageSize(int width, int height, int max_width, int max_height,
                  int* fitted_width, int* fitted_height) {
    if (width <= 0 || height <= 0 || max_width <= 0 || max_height <= 0 ||
        !fitted_width || !fitted_height) return false;
    if (width <= max_width && height <= max_height) {
        *fitted_width = width; *fitted_height = height;
    } else if (int64_t(width) * max_height > int64_t(height) * max_width) {
        *fitted_width = max_width;
        *fitted_height = std::max<int64_t>(1, int64_t(height) * max_width / width);
    } else {
        *fitted_height = max_height;
        *fitted_width = std::max<int64_t>(1, int64_t(width) * max_height / height);
    }
    return true;
}

bool ReadImageDimensions(ImageHeaderRead read, void* context, int* width, int* height) {
    if (!read || !width || !height) return false;
    size_t consumed = 0;
    const auto exact = [&](uint8_t* bytes, size_t size) {
        // 尺寸探测最多读取 256 KiB，避免异常 JPEG 元数据长时间占用 SD。
        if (size > 256 * 1024 - consumed) return false;
        while (size) {
            const size_t got = read(context, bytes, size);
            if (!got || got > size) return false;
            consumed += got; bytes += got; size -= got;
        }
        return true;
    };
    uint8_t bytes[24];
    if (!exact(bytes, 8)) return false;
    if (!std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        if (!exact(bytes + 8, 16) || std::memcmp(bytes + 12, "IHDR", 4)) return false;
        const auto be32 = [](const uint8_t* p) {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        };
        const uint32_t w = be32(bytes + 16), h = be32(bytes + 20);
        if (!w || !h || w > UINT16_MAX || h > UINT16_MAX) return false;
        *width = w; *height = h;
        return true;
    }
    if (!std::memcmp(bytes, "A2I1", 4) || !std::memcmp(bytes, "A2L8", 4) ||
        !std::memcmp(bytes, "EBGR", 4)) {
        *width = bytes[4] | (unsigned(bytes[5]) << 8);
        *height = bytes[6] | (unsigned(bytes[7]) << 8);
        return *width > 0 && *height > 0;
    }
    if (bytes[0] != 0xff || bytes[1] != 0xd8) return false;
    size_t prefix = 2;
    const auto next = [&](uint8_t* out) {
        if (prefix < 8) { *out = bytes[prefix++]; return true; }
        return exact(out, 1);
    };
    uint8_t marker = 0, byte = 0;
    for (;;) {
        if (!next(&byte) || byte != 0xff) return false;
        do { if (!next(&marker)) return false; } while (marker == 0xff);
        if (marker == 0xda || marker == 0xd9 || marker == 0) return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (!next(&byte)) return false;
        unsigned length = unsigned(byte) << 8;
        if (!next(&byte)) return false;
        length |= byte;
        if (length < 2) return false;
        const bool sof = (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                          marker != 0xc8 && marker != 0xcc);
        if (sof) {
            if (length < 8) return false;
            uint8_t dimensions[5];
            for (auto& value : dimensions) if (!next(&value)) return false;
            *height = unsigned(dimensions[1]) << 8 | dimensions[2];
            *width = unsigned(dimensions[3]) << 8 | dimensions[4];
            return *width > 0 && *height > 0;
        }
        unsigned remaining = length - 2;
        while (remaining && prefix < 8) { ++prefix; --remaining; }
        uint8_t skipped[256];
        while (remaining) {
            const unsigned size = std::min<unsigned>(remaining, sizeof(skipped));
            if (!exact(skipped, size)) return false;
            remaining -= size;
        }
    }
}

} // namespace reader
