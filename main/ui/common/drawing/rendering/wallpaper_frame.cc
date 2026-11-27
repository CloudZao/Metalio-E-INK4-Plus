#include <cstring>
#include <memory>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "wallpaper_frame.h"
#include "infra/image/image_tone.h"
#include "business/reader/reader_types.h"
#include "ui/common/drawing/adapter/epd_refresh.h"

static std::unique_ptr<uint8_t, decltype(&heap_caps_free)> s_base(nullptr, heap_caps_free);
static lv_point_t s_size{};

static size_t FrameBytes() {
    return size_t(EpdGetFrameBuffer().stride) * EpdGetFrameBuffer().height;
}

bool WallpaperFrame_Capture() {
    if (s_base) {
        return true;
    }
    const uint8_t* frame = EpdRenderBuffer();
    if (!frame) {
        return false;
    }
    s_base.reset(static_cast<uint8_t*>(heap_caps_malloc(FrameBytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!s_base) {
        return false;
    }
    std::memcpy(s_base.get(), frame, FrameBytes());
    // 底图保持原始帧。壁纸按完整竖屏范围解码，不随当前界面方向改变。
    const auto layout = EpdGetFrameBuffer();
    s_size = {layout.height, layout.width};
    return true;
}

bool WallpaperFrame_HasBase() {
    return s_base != nullptr;
}

lv_point_t WallpaperFrame_Size() {
    return s_size;
}

bool WallpaperFrame_Restore() {
    uint8_t* frame = EpdRenderBuffer();
    if (!s_base || !frame) {
        return false;
    }
    std::memcpy(frame, s_base.get(), FrameBytes());
    return true;
}

bool WallpaperFrame_Draw(const RasterImage& image, const std::vector<uint8_t>& alpha) {
    if (image.empty() || image.width > s_size.x || image.height > s_size.y ||
        (!alpha.empty() && alpha.size() != image.pixels.size()) || !WallpaperFrame_Restore()) {
        return false;
    }
    const int left = (s_size.x - image.width) / 2;
    const int top = (s_size.y - image.height) / 2;
    uint8_t* frame = EpdRenderBuffer();
    const auto layout = EpdGetFrameBuffer();
    const int stride = layout.stride;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const size_t index = size_t(y) * image.width + x;
            const unsigned opacity = alpha.empty() ? 255 : alpha[index];
            if (opacity == 0) {
                continue;
            }
            // 竖屏：逻辑行写入物理列，逻辑列从物理底边向上。
            const int px = top + y;
            const int py = layout.height - 1 - (left + x);
            if (px < 0 || py < 0 || px >= layout.width || py >= layout.height) {
                return false;
            }
            if (layout.bits_per_pixel == 1) {
                auto& byte = frame[size_t(py) * stride + px / 8];
                const uint8_t mask = 0x80u >> (px & 7);
                unsigned value = DarkenImagePixel(image.pixels[index]);
                const unsigned background = (byte & mask) ? 255 : 0;
                value = (value * opacity + background * (255 - opacity) + 127) / 255;
                static constexpr uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                if (value > unsigned(bayer[py & 3][px & 3]) * 16 + 7) {
                    byte |= mask;
                } else {
                    byte &= static_cast<uint8_t>(~mask);
                }
                continue;
            }
            uint8_t& pair = frame[size_t(py) * stride + px / 2];
            const int shift = (px & 1) * 4;
            unsigned value = DarkenImagePixel(image.pixels[index]);
            if (opacity != 255) {
                const unsigned background = ((pair >> shift) & 15) * 17;
                value = (value * opacity + background * (255 - opacity) + 127) / 255;
            }
            const unsigned gray = (value * 15 + 127) / 255;
            pair = (pair & ~(15 << shift)) | (gray << shift);
        }
        if ((y & 63) == 0) {
            taskYIELD();
        }
    }
    return true;
}

void WallpaperFrame_Clear() {
    s_base.reset();
    s_size = {};
}
