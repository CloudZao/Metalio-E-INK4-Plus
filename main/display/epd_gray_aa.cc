/**
 * @file epd_gray_aa.cc
 * @brief 阅读真 4 灰 AA：L8 sidecar 与 Overlay 掩码打包实现
 */
#include "epd_gray_aa.h"
#include "display_orient.h"
#include "epd_i1_glyph_thin.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {

constexpr const char* TAG = "EpdGrayAa";

int s_w = 0;
int s_h = 0;
size_t s_l8_bytes = 0;
size_t s_plane_bytes = 0;
uint8_t* s_l8 = nullptr;
bool s_frame_active = false;

void Plot(int32_t x, int32_t y, uint8_t mask_val) {
    if (!s_frame_active || s_l8 == nullptr || mask_val == 0) {
        return;
    }
    if (x < 0 || y < 0 || x >= s_w || y >= s_h) {
        return;
    }
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(s_w) + static_cast<size_t>(x);
    if (mask_val > s_l8[i]) {
        s_l8[i] = mask_val;
    }
}

}  // namespace

bool epd_gray_aa_ensure(int width, int height) {
    if (width <= 0 || height <= 0 || (width & 7) != 0) {
        return false;
    }
    if (s_l8 != nullptr && s_w == width && s_h == height) {
        return true;
    }
    if (s_l8 != nullptr) {
        heap_caps_free(s_l8);
        s_l8 = nullptr;
    }
    s_w = width;
    s_h = height;
    s_l8_bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    s_plane_bytes = s_l8_bytes / 8;
    s_l8 = static_cast<uint8_t*>(
        heap_caps_malloc(s_l8_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_l8 == nullptr) {
        ESP_LOGE(TAG, "sidecar alloc %u failed", static_cast<unsigned>(s_l8_bytes));
        s_w = s_h = 0;
        s_l8_bytes = s_plane_bytes = 0;
        return false;
    }
    std::memset(s_l8, 0, s_l8_bytes);
    ESP_LOGI(TAG, "sidecar %dx%d L8=%uKB", width, height,
             static_cast<unsigned>(s_l8_bytes / 1024));
    return true;
}

void epd_gray_aa_begin_frame(void) {
    if (s_l8 == nullptr || s_l8_bytes == 0) {
        s_frame_active = false;
        return;
    }
    if (!epd_i1_glyph_aa_enabled) {
        s_frame_active = false;
        return;
    }
    std::memset(s_l8, 0, s_l8_bytes);
    s_frame_active = true;
}

void epd_gray_aa_plot_logical(int32_t lx, int32_t ly, uint8_t mask_val) {
    if (!s_frame_active || s_l8 == nullptr || s_w <= 0 || s_h <= 0) {
        return;
    }
    int px = 0;
    int py = 0;
    DisplayUiLogicalToPanel(static_cast<int>(lx), static_cast<int>(ly), &px, &py);
    Plot(px, py, mask_val);
}

void epd_gray_aa_pack_overlay_plane(uint8_t* out, int plane_msb) {
    if (out == nullptr || s_l8 == nullptr || s_plane_bytes == 0) {
        return;
    }
    const bool want_msb = plane_msb != 0;
    for (size_t bi = 0; bi < s_plane_bytes; ++bi) {
        uint8_t byte = 0;
        for (int bit = 7; bit >= 0; --bit) {
            const size_t pix = bi * 8u + static_cast<size_t>(7 - bit);
            if (pix >= s_l8_bytes) {
                break;
            }
            const uint8_t cov = s_l8[pix];
            int on = 0;
            if (cov > 0 && cov < 128) {
                // ~85 → 浅灰 01：仅 MSB
                on = want_msb ? 1 : 0;
            } else if (cov >= 128 && cov < 255) {
                // ~170 → 深灰 11：LSB+MSB
                on = 1;
            }
            if (on) {
                byte |= static_cast<uint8_t>(1u << bit);
            }
        }
        out[bi] = byte;
    }
}
