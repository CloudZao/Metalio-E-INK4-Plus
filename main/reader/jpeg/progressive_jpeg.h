#pragma once

#include <cstddef>
#include <cstdint>
#include "../reader_types.h"

namespace reader {
using ImageDecodeCancelled = bool (*)(void*); // 返回 true 时取消当前解码

/** @brief 判断 JPEG 是否采用渐进式 SOF2 编码 */
bool IsProgressiveJpeg(const uint8_t* data, size_t len);

/**
 * @brief 在 PSRAM 预算内解码渐进式 JPEG 并缩放为 L8，取消或失败时释放临时缓冲
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeProgressiveJpeg(const uint8_t* data, size_t len, int max_w, int max_h,
                           RasterImage& out, ImageDecodeCancelled cancelled, void* context,
                           ImageDecodeError* error = nullptr);
}
