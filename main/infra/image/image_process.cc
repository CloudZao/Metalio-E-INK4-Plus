#include <algorithm>
#include <cmath>
#include "image_process.h"

bool GetImageProcessingLayout(int width, int height, const ImageProcessingOptions& options,
                              ImageProcessingLayout& layout) {
    layout = {};
    if (width < 1 || height < 1 || width > 1600 || height > 1600 || int64_t(width) * height > 1024 * 1024 ||
        (options.levels != 2 && options.levels != 8 && options.levels != 16) ||
        options.contrast < -100 || options.contrast > 100 || options.brightness < -100 || options.brightness > 100 ||
        options.exposure < -200 || options.exposure > 200 || options.shadows < -100 || options.shadows > 100 ||
        options.highlights < -100 || options.highlights > 100 || options.gamma < 25 || options.gamma > 400 ||
        options.sharpness < -100 || options.sharpness > 100 || options.threshold > 255 ||
        options.rotation > 270 || options.rotation % 90 ||
        options.crop_x >= 10000 || options.crop_y >= 10000 || !options.crop_width || !options.crop_height ||
        options.crop_width > 10000 - options.crop_x || options.crop_height > 10000 - options.crop_y) {
        return false;
    }
    layout.x = width * options.crop_x / 10000;
    layout.y = height * options.crop_y / 10000;
    layout.width = std::max(1, int(width * (options.crop_x + options.crop_width) / 10000) - layout.x);
    layout.height = std::max(1, int(height * (options.crop_y + options.crop_height) / 10000) - layout.y);
    const bool sideways = options.rotation == 90 || options.rotation == 270;
    layout.output_width = sideways ? layout.height : layout.width;
    layout.output_height = sideways ? layout.width : layout.height;
    return true;
}

static void BuildImageToneTable(const ImageProcessingOptions& options, uint8_t (&tone)[256]) {
    const float gain = options.exposure == 0 ? 1.0f : std::exp2(float(options.exposure) / 100.0f);
    for (unsigned i = 0; i < 256; ++i) {
        int value = i;
        if (options.exposure != 0) {
            // 软件 EV 在近似 sRGB 线性域计算，不改变传感器，也不恢复源图已经丢失的细节。
            const float encoded = float(i) / 255.0f;
            const float linear = encoded <= 0.04045f ? encoded / 12.92f :
                std::pow((encoded + 0.055f) / 1.055f, 2.4f);
            const float adjusted = std::min(1.0f, linear * gain);
            const float output = adjusted <= 0.0031308f ? adjusted * 12.92f :
                1.055f * std::pow(adjusted, 1.0f / 2.4f) - 0.055f;
            value = int(output * 255.0f + 0.5f);
        }
        value = std::clamp(value + options.brightness * 255 / 100, 0, 255);
        // 两个权重分别偏向暗部和亮部，黑白端点不移动；按顺序计算并限制有效范围。
        value = std::clamp(value + int(int64_t(options.shadows) * 2 * value * (255 - value) * (255 - value) /
                                      (100 * 255 * 255)), 0, 255);
        value = std::clamp(value + int(int64_t(options.highlights) * 2 * value * value * (255 - value) /
                                      (100 * 255 * 255)), 0, 255);
        value = std::clamp((value - 128) * (100 + options.contrast) / 100 + 128, 0, 255);
        if (options.gamma != 100) {
            value = std::clamp(int(std::pow(float(value) / 255.0f, 100.0f / options.gamma) * 255.0f + 0.5f), 0, 255);
        }
        tone[i] = options.invert ? 255 - value : value;
    }
}

bool ProcessImageL8(const uint8_t* source, size_t source_size, int width, int height,
                     const ImageProcessingOptions& options, uint8_t* target, size_t target_size) {
    ImageProcessingLayout layout;
    if (!source || !target || source == target || !GetImageProcessingLayout(width, height, options, layout) ||
        source_size < size_t(width) * height || target_size < size_t(layout.output_width) * layout.output_height) {
        return false;
    }
    uint8_t tone[256];
    BuildImageToneTable(options, tone);
    static constexpr uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    for (int y = 0; y < layout.height; ++y) {
        for (int x = 0; x < layout.width; ++x) {
            const int sx = layout.x + (options.mirror_x ? layout.width - 1 - x : x);
            const int sy = layout.y + (options.mirror_y ? layout.height - 1 - y : y);
            int value = tone[source[size_t(sy) * width + sx]];
            if (options.sharpness != 0) {
                int sum = 0;
                // 核边界限制在裁剪区域内，不从已裁去的区域引入像素。
                for (int ky = -1; ky <= 1; ++ky) {
                    const int py = std::clamp(sy + ky, layout.y, layout.y + layout.height - 1);
                    for (int kx = -1; kx <= 1; ++kx) {
                        const int px = std::clamp(sx + kx, layout.x, layout.x + layout.width - 1);
                        sum += tone[source[size_t(py) * width + px]];
                    }
                }
                const int average = (sum + 4) / 9;
                value = options.sharpness < 0 ? value + (average - value) * -options.sharpness / 100 :
                    std::clamp(value + (value - average) * options.sharpness / 50, 0, 255);
            }
            int dx = x, dy = y;
            switch (options.rotation) {
                case 90:
                    dx = layout.height - 1 - y;
                    dy = x;
                    break;
                case 180:
                    dx = layout.width - 1 - x;
                    dy = layout.height - 1 - y;
                    break;
                case 270:
                    dx = y;
                    dy = layout.width - 1 - x;
                    break;
            }
            if (options.levels == 2) {
                const unsigned cutoff = options.dither ? (unsigned(bayer[(dy & 3) * 4 + (dx & 3)]) * 2 + 1) * 255 / 32 :
                    options.threshold;
                value = unsigned(value) > cutoff ? 255 : 0;
            } else {
                const unsigned step = (unsigned(value) * (options.levels - 1) + 127) / 255;
                value = (step * 255 + (options.levels - 1) / 2) / (options.levels - 1);
            }
            target[size_t(dy) * layout.output_width + dx] = value;
        }
    }
    return true;
}
