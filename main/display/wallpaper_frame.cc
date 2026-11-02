#include "wallpaper_frame.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "epdiy.h"
#include "image_tone.h"
#include "reader_types.h"

#include <cstring>
#include <memory>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
std::unique_ptr<uint8_t, decltype(&heap_caps_free)> s_base(nullptr, heap_caps_free);
lv_point_t s_size{};
struct NativePoint {
    int x = 0; // 面板列坐标
    int y = 0; // 面板行坐标
};
NativePoint s_origin{}, s_step_x{}, s_step_y{};
size_t FrameBytes() { return size_t(epd_width() / 2) * epd_height(); }
}

bool WallpaperFrame_Capture() {
    if (s_base) return true;
    const uint8_t* frame = EpdRenderBuffer();
    if (!frame) return false;
    s_base.reset(static_cast<uint8_t*>(heap_caps_malloc(FrameBytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!s_base) return false;
    std::memcpy(s_base.get(), frame, FrameBytes());
    DisplayUiLogicalToPanel(0, 0, &s_origin.x, &s_origin.y);
    DisplayUiLogicalToPanel(1, 0, &s_step_x.x, &s_step_x.y);
    DisplayUiLogicalToPanel(0, 1, &s_step_y.x, &s_step_y.y);
    s_step_x.x -= s_origin.x;
    s_step_x.y -= s_origin.y;
    s_step_y.x -= s_origin.x;
    s_step_y.y -= s_origin.y;
    // 叠图与全屏壁纸使用相同面板边界，保留原界面方向而不触发 LVGL 重排。
    s_size = {s_step_x.x ? epd_width() : epd_height(),
              s_step_y.x ? epd_width() : epd_height()};
    s_origin.x = (s_step_x.x < 0 || s_step_y.x < 0) ? epd_width() - 1 : 0;
    s_origin.y = (s_step_x.y < 0 || s_step_y.y < 0) ? epd_height() - 1 : 0;
    return true;
}

bool WallpaperFrame_HasBase() { return s_base != nullptr; }
lv_point_t WallpaperFrame_Size() { return s_size; }

bool WallpaperFrame_Restore() {
    uint8_t* frame = EpdRenderBuffer();
    if (!s_base || !frame) return false;
    std::memcpy(frame, s_base.get(), FrameBytes());
    return true;
}

bool WallpaperFrame_Draw(const reader::RasterImage& image, const std::vector<uint8_t>& alpha) {
    if (image.empty() || image.width > s_size.x || image.height > s_size.y ||
        (!alpha.empty() && alpha.size() != image.pixels.size()) || !WallpaperFrame_Restore()) return false;
    const int left = (s_size.x - image.width) / 2;
    const int top = (s_size.y - image.height) / 2;
    uint8_t* frame = EpdRenderBuffer();
    const int stride = epd_width() / 2;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const size_t index = size_t(y) * image.width + x;
            const unsigned opacity = alpha.empty() ? 255 : alpha[index];
            if (opacity == 0) continue;
            const int px = s_origin.x + (left + x) * s_step_x.x + (top + y) * s_step_y.x;
            const int py = s_origin.y + (left + x) * s_step_x.y + (top + y) * s_step_y.y;
            if (px < 0 || py < 0 || px >= epd_width() || py >= epd_height()) return false;
            uint8_t& pair = frame[size_t(py) * stride + px / 2];
            const int shift = (px & 1) * 4;
            unsigned value = reader::DarkenImagePixel(image.pixels[index]);
            if (opacity != 255) {
                const unsigned background = ((pair >> shift) & 15) * 17;
                value = (value * opacity + background * (255 - opacity) + 127) / 255;
            }
            const unsigned gray = (value * 15 + 127) / 255;
            pair = (pair & ~(15 << shift)) | (gray << shift);
        }
        if ((y & 63) == 0) taskYIELD();
    }
    return true;
}

void WallpaperFrame_Clear() {
    s_base.reset();
    s_size = {};
}
