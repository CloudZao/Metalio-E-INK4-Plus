#pragma once
#include <stdint.h>
#include <string.h>

// I1 is MSB-first. Preserve neighboring pixels in the first/last byte.
static inline void epd_i1_solid_fill(uint8_t* dst, int32_t width, int32_t height, int32_t stride, unsigned offset,
                                     int white) {
    if (width <= 0 || height <= 0) {
        return;
    }
    offset &= 7;
    for (int32_t y = 0; y < height; ++y) {
        uint8_t* row = dst;
        unsigned remaining = (unsigned)width;
        if (offset) {
            const unsigned count = remaining < 8 - offset ? remaining : 8 - offset;
            const uint8_t mask = (uint8_t)((0xffu >> offset) & (0xffu << (8 - offset - count)));
            *row = white ? (uint8_t)(*row | mask) : (uint8_t)(*row & ~mask);
            ++row;
            remaining -= count;
        }
        const unsigned bytes = remaining / 8;
        if (bytes) {
            memset(row, white ? 0xff : 0, bytes);
        }
        row += bytes;
        const unsigned tail = remaining % 8;
        if (tail) {
            const uint8_t mask = (uint8_t)(0xffu << (8 - tail));
            *row = white ? (uint8_t)(*row | mask) : (uint8_t)(*row & ~mask);
        }
        dst += stride;
    }
}

// Public LVGL blend hook, injected only into the I1 translation unit.
// Opaque L8 pages: pack eight thresholded pixels per store, preserving clipped edge bits.
static inline __attribute__((optimize("O3"))) void epd_i1_l8_copy(uint8_t* dst, const uint8_t* src, int32_t width,
                                                                  int32_t height, int32_t dst_stride,
                                                                  int32_t src_stride, unsigned offset,
                                                                  unsigned threshold) {
    offset &= 7;
    for (int32_t y = 0; y < height; ++y) {
        int32_t x = 0;
        if (offset) {
            const int32_t n = width < (int32_t)(8 - offset) ? width : (int32_t)(8 - offset);
            uint8_t bits = 0, mask = 0;
            for (; x < n; ++x) {
                const uint8_t bit = (uint8_t)(1u << (7 - offset - x));
                mask |= bit;
                if (src[x] > threshold) {
                    bits |= bit;
                }
            }
            dst[0] = (dst[0] & ~mask) | bits;
        }
        for (; x + 8 <= width; x += 8) {
            uint8_t bits = 0;
            for (unsigned i = 0; i < 8; ++i) {
                bits |= (uint8_t)((src[x + i] > threshold) << (7 - i));
            }
            dst[(x + offset) / 8] = bits;
        }
        if (x < width) {
            uint8_t bits = 0, mask = 0;
            const int32_t byte = (x + offset) / 8;
            for (; x < width; ++x) {
                const uint8_t bit = (uint8_t)(1u << (7 - ((x + offset) & 7)));
                mask |= bit;
                if (src[x] > threshold) {
                    bits |= bit;
                }
            }
            dst[byte] = (dst[byte] & ~mask) | bits;
        }
        dst += dst_stride;
        src += src_stride;
    }
}
#define LV_DRAW_SW_L8_BLEND_NORMAL_TO_I1(dsc)                                                                    \
    (epd_i1_l8_copy((uint8_t*)(dsc)->dest_buf, (dsc)->src_buf, (dsc)->dest_w, (dsc)->dest_h, (dsc)->dest_stride, \
                    (dsc)->src_stride, (unsigned)((dsc)->relative_area.x1 % 8), LV_DRAW_SW_I1_LUM_THRESHOLD),    \
     LV_RESULT_OK)

#define LV_DRAW_SW_COLOR_BLEND_TO_I1(dsc)                                                           \
    (epd_i1_solid_fill((uint8_t*)(dsc)->dest_buf, (dsc)->dest_w, (dsc)->dest_h, (dsc)->dest_stride, \
                       (unsigned)((dsc)->relative_area.x1 % 8),                                     \
                       lv_color_luminance((dsc)->color) / (LV_DRAW_SW_I1_LUM_THRESHOLD + 1) != 0),  \
     LV_RESULT_OK)
