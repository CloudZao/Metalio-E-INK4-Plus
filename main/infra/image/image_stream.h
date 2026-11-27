#pragma once

#include <cstdio>
#include "image_decode.h"

struct ImageDecodeInput {
    void* context = nullptr;                                               // 借用输入状态，不接管其生命周期
    size_t (*read)(void* context, uint8_t* bytes, size_t size) = nullptr;  // 返回已消费字节数，bytes 为空时跳过数据
};

/** @brief 从借用字节流解码普通 JPEG，取消及错误结果由调用方接收。 */
bool DecodeJpegStreamToL8(const ImageDecodeInput& input, int max_w, int max_h, RasterImage& out,
                          ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                          ImageDecodeError* error = nullptr);

/** @brief 从借用字节流逐行解码 PNG，不缓存完整压缩输入或源像素。 */
bool DecodePngStreamToL8(const ImageDecodeInput& input, int max_w, int max_h, RasterImage& out,
                         ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                         ImageDecodeError* error = nullptr);

/**
 * @brief 从借用文件流式解码普通 JPEG，不缓存完整压缩输入，支持取消
 */
bool DecodeJpegFileToL8(FILE* input, int max_w, int max_h, RasterImage& out, ImageDecodeCancelled cancelled,
                        void* context, ImageDecodeError* error, StorageError* storage_error = nullptr);

/**
 * @brief 内存 PNG 按行缩小到 L8
 * @param error 可选错误码输出，成功时为 kNone
 * @note 支持非交错 8/16 位灰度及真彩、8 位索引色；行缓冲按实际宽度分配。
 *       max_w、max_h 均为 0 时保留原尺寸；其他调用的 L8 输出上限为 1 MiB。
 */
bool DecodePngMemoryToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out,
                         ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                         ImageDecodeError* error = nullptr);

/**
 * @brief 从文件流式解码 PNG，不缓存完整压缩文件，支持调用方取消
 * @param error 可选错误码输出，成功时为 kNone
 * @note alpha 非空时输出图片自身透明度；否则合成到白底。无透明信息时 alpha 输出为空。
 */
bool DecodePngFileToL8(const char* path, int max_w, int max_h, RasterImage& out,
                       ImageDecodeCancelled cancelled = nullptr, void* context = nullptr,
                       std::vector<uint8_t>* alpha = nullptr, ImageDecodeError* error = nullptr, StorageError* storage_error = nullptr);
