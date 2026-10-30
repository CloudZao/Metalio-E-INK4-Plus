#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

using ImageHeaderRead = size_t (*)(void* context, uint8_t* bytes, size_t size);

/** @brief 流式读取 PNG、JPEG 或设备图片封装的尺寸，不解码像素 */
bool ReadImageDimensions(ImageHeaderRead read, void* context, int* width, int* height);

/** @brief 按宽高限制等比缩小，不放大小图；无效尺寸返回 false */
bool FitImageSize(int width, int height, int max_width, int max_height,
                  int* fitted_width, int* fitted_height);

} // namespace reader
