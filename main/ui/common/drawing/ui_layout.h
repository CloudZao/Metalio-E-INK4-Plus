#pragma once

#include <algorithm>
#include <cstdint>

struct UiSize {
    int32_t width;   // 逻辑宽度，单位像素
    int32_t height;  // 逻辑高度，单位像素
};

/** @brief 按可用长度计算等宽列或等高行的数量，空间不足时保留一项。 */
static inline int UiLayoutCount(int width, int minimum_width, int gap, int maximum_columns) {
    gap = std::max(0, gap);
    minimum_width = std::max(1, minimum_width);
    maximum_columns = std::max(1, maximum_columns);
    return std::clamp((std::max(0, width) + gap) / (minimum_width + gap), 1, maximum_columns);
}

/** @brief 将图片按比例放入逻辑内容区，可选择是否放大小图。 */
static inline UiSize UiLayoutFitImage(UiSize available, UiSize source, bool upscale = true) {
    if (available.width <= 0 || available.height <= 0 || source.width <= 0 || source.height <= 0)
        return {0, 0};
    if (!upscale && source.width <= available.width && source.height <= available.height) {
        return source;
    }
    if (static_cast<int64_t>(available.width) * source.height <=
        static_cast<int64_t>(available.height) * source.width) {
        return {available.width,
                std::max<int>(1, static_cast<int64_t>(available.width) * source.height / source.width)};
    }
    return {std::max<int>(1, static_cast<int64_t>(available.height) * source.width / source.height), available.height};
}
