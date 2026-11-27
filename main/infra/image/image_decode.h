#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "raster_image.h"
#include "infra/storage/storage_error.h"

using ImageDecodeCancelled = bool (*)(void*);  // 返回 true 时取消当前解码

enum class ImageDecodeError : uint8_t {
    kNone = 0,             // 解码成功
    kInvalidData = 1,      // 数据损坏或不完整
    kUnsupported = 2,      // 格式、编码或尺寸不支持
    kNoMemory = 3,         // 内存分配失败或超出解码预算
    kFileIo = 4,           // 文件打开或读取失败
    kCancelled = 5,        // 调用方取消
    kInvalidArgument = 6,  // 解码参数无效
    kFailed = 7,           // 解码器未细分的失败
};

/**
 * @brief 解码图片为 L8；max_w、max_h 均为 0 时保留原始尺寸，支持取消回调
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeImageToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out,
                     ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                     ImageDecodeError* error = nullptr);

/**
 * @brief 从文件解码为 L8；max_w、max_h 均为 0 时保留原始尺寸，支持取消回调
 * @param storage_error 可选存储错误，在原始 I/O 失败点保存，不包含路径
 * @param error 可选错误码输出，成功时为 kNone
 */
bool DecodeImageFileToL8(const char* path, int max_w, int max_h, RasterImage& out,
                         ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                         ImageDecodeError* error = nullptr, StorageError* storage_error = nullptr);

/** @brief 等比缩小图片并保留文件透明度；不放大小图，不去除白底。 */
bool DecodeImageFileOverlay(const char* path, int max_w, int max_h, RasterImage& out, std::vector<uint8_t>& alpha,
                            ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                            ImageDecodeError* error = nullptr, StorageError* storage_error = nullptr);

/** @brief 窥探旧 A2I1 缓冲宽高；失败返回 false */
bool PeekA2i1Size(const uint8_t* data, size_t len, int* w, int* h);

/** @brief 将已有 L8 缩小贴入 max 框（不放大）；src/out 可同对象 */
bool ScaleRasterToFit(const RasterImage& src, int max_w, int max_h, RasterImage& out);

/** @brief 对边均有明显白边时裁到墨迹包围盒，保留原有亮度。 */
void TrimRasterLetterbox(RasterImage& image);

/** @brief 先水平镜像再顺时针旋转指定次数；源与目标可为同一对象。 */
void OrientRasterImage(const RasterImage& source, uint8_t rotation, bool mirror, RasterImage& out);

/** @brief 校验旧 A2I1 灰度封装的尺寸、步长及数据长度。 */
bool ValidateA2i1Bytes(const uint8_t* data, size_t len);

/** @brief 校验 A2L8 灰度封装的尺寸及像素长度。 */
bool ValidateA2l8Bytes(const uint8_t* data, size_t len);

/** @brief 将 L8 图像编码为兼容 A2UI 的 A2L8 灰度封装。 */
bool EncodeL8ToA2l8(const RasterImage& image, std::vector<uint8_t>& out);

/** @brief 从内存探测支持的图像尺寸，不解码像素。 */
bool PeekRasterSize(const uint8_t* data, size_t len, int* width, int* height);

/** @brief 从文件探测支持的图像尺寸，不缓存完整输入。 */
bool PeekRasterFileSize(const char* path, int* width, int* height);
