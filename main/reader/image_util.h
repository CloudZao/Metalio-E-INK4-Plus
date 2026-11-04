#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "reader_types.h"
#include "jpeg/progressive_jpeg.h"

namespace reader {

/**
 * @brief 解码图片为 L8；max_w、max_h 均为 0 时保留原始尺寸，支持取消回调
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeImageToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out,
                     ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                     ImageDecodeError* error = nullptr);

/**
 * @brief 从文件解码为 L8；max_w、max_h 均为 0 时保留原始尺寸，支持取消回调
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeImageFileToL8(const char* path, int max_w, int max_h, RasterImage& out,
                     ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                     ImageDecodeError* error = nullptr);

/** @brief 等比缩小图片并保留文件透明度；不放大小图，不去除白底。 */
bool DecodeImageFileOverlay(const char* path, int max_w, int max_h, RasterImage& out,
                           std::vector<uint8_t>& alpha, ImageDecodeCancelled cancelled = nullptr,
                           void* context = nullptr, ImageDecodeError* error = nullptr);

/** @brief 窥探旧 A2I1 缓冲宽高；失败返回 false */
bool PeekA2i1Size(const uint8_t* data, size_t len, int* w, int* h);

/** @brief 将已有 L8 缩小贴入 max 框（不放大）；src/out 可同对象 */
bool ScaleRasterToFit(const RasterImage& src, int max_w, int max_h, RasterImage& out);

// A2I1 文件/缓冲校验（与 tools/a2i1、a2ui 布局一致）。
bool ValidateA2i1Bytes(const uint8_t* data, size_t len);

// 原子写文件：path.tmp → fflush → unlink(path) → rename；写失败清 .tmp；
// rename 失败保留 .tmp（勿删），便于调用方回退读取。
bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len);

// Gray content envelope, compatible with the existing A2UI A2L8 payload.
bool ValidateA2l8Bytes(const uint8_t* data, size_t len);
bool EncodeL8ToA2l8(const RasterImage& image, std::vector<uint8_t>& out);
bool PeekRasterSize(const uint8_t* data, size_t len, int* width, int* height);
bool PeekRasterFileSize(const char* path, int* width, int* height);
}  // namespace reader
