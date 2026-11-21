#pragma once
#include <cstddef>
#include <cstdint>

namespace plus_hal_detail {
constexpr int kWidth = 1216, kHeight = 684;
constexpr size_t kMonoBytes = size_t(kWidth) * kHeight / 8;
constexpr size_t kGrayBytes = size_t(kWidth) * kHeight / 2;

inline bool SupportedSize(int width, int height) {
    return (width == 800 && height == 480) ||
           (width == kWidth && height == kHeight);
}

// Caller owns both complete buffers. Physical panel even X is LOW nibble;
// input is row-major MSB first, 1=black. No LVGL format/polarity assumption.
inline void ExpandMono(const uint8_t* mono, int width, int height, uint8_t* gray) {
    const size_t stride = size_t(width) / 8;
    for (int y = 0; y < kHeight; ++y) {
        const int sy = y * height / kHeight;
        const uint8_t* row = mono + size_t(sy) * stride;
        uint8_t* out = gray + size_t(y) * (kWidth / 2);
        for (int x = 0; x < kWidth; x += 2) {
            const int sx0 = x * width / kWidth;
            const int sx1 = (x + 1) * width / kWidth;
            const uint8_t lo = (row[sx0 / 8] & (0x80u >> (sx0 & 7))) ? 0 : 15;
            const uint8_t hi = (row[sx1 / 8] & (0x80u >> (sx1 & 7))) ? 0 : 15;
            out[x / 2] = uint8_t(lo | (hi << 4));
        }
    }
}
} // namespace plus_hal_detail
