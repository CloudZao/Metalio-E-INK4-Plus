#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "../reader_types.h"

namespace reader {
using ImageDecodeCancelled = bool (*)(void*); // 返回 true 时取消当前解码

/** @brief 判断 JPEG 是否采用渐进式 SOF2 编码 */
bool IsProgressiveJpeg(const uint8_t* data, size_t len);

/** @brief 从已打开文件探测渐进式 JPEG，不缓存完整输入，调用后游标位置不保留 */
bool IsProgressiveJpegFile(FILE* input, size_t len, ImageDecodeCancelled cancelled = nullptr,
                           void* context = nullptr);

/**
 * @brief 在 PSRAM 预算内解码渐进式 JPEG 并缩放为 L8，取消或失败时释放临时缓冲
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeProgressiveJpeg(const uint8_t* data, size_t len, int max_w, int max_h,
                           RasterImage& out, ImageDecodeCancelled cancelled, void* context,
                           ImageDecodeError* error = nullptr);

/**
 * @brief 从借用文件解码渐进式 JPEG，大图使用有界条带及 SD 系数后备文件
 */
bool DecodeProgressiveJpegFile(FILE* input, size_t len, int max_w, int max_h,
                               RasterImage& out, ImageDecodeCancelled cancelled, void* context,
                               ImageDecodeError* error = nullptr);
}
