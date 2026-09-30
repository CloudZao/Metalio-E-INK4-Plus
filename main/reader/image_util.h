#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "reader_types.h"

namespace reader {

// 将 PNG/JPEG 内存图解码并缩放到 max_w/max_h，输出 L8（Bayer 抖动，适配 I1 墨水屏）。
bool DecodeImageToL8(const uint8_t* data, size_t len, int max_w, int max_h, RasterImage& out);

// 从文件路径解码（TXT 旁路插图、书库 .a2i1 封面等）。
bool DecodeImageFileToL8(const char* path, int max_w, int max_h, RasterImage& out);

/** @brief 窥探 A2I1 宽高（文件或内存）；失败返回 false */
bool PeekA2i1Size(const uint8_t* data, size_t len, int* w, int* h);
bool PeekA2i1FileSize(const char* path, int* w, int* h);

/** @brief 将已有 L8 缩小贴入 max 框（不放大）；src/out 可同对象 */
bool ScaleRasterToFit(const RasterImage& src, int max_w, int max_h, RasterImage& out);

// A2I1 文件/缓冲校验（与 tools/a2i1、a2ui 布局一致）。
bool ValidateA2i1Bytes(const uint8_t* data, size_t len);

// L8（0=黑 255=白）编码为 A2I1 文件字节。
bool EncodeL8ToA2i1(const RasterImage& l8, std::vector<uint8_t>& out);

// 原子写文件：path.tmp → fflush → unlink(path) → rename；写失败清 .tmp；
// rename 失败保留 .tmp（勿删），便于调用方回退读取。
bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len);

}  // namespace reader
