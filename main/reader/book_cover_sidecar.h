#pragma once

#include <cstddef>

#include "reader_types.h"

namespace reader {

// 书库旁路封面：同书文件 stem，扩展名 .a2i1（例：元尊.ebook -> 元尊.a2i1）。
// UI 只读旁路；无旁路或尺寸不够时后台抽内嵌并按权威档回写。

// 由电子书绝对路径生成旁路 .a2i1 路径；失败返回 false。
bool BookCoverSidecarPath(const char* book_path, char* out, size_t out_sz);

// 旁路是否足够覆盖 need 框；不够时走重抽升级。
bool BookCoverSidecarAdequate(const char* book_path, int need_w, int need_h);

// 从 EPUB/.ebook 提取封面并原子写入旁路 A2I1；无封面或失败返回 false。
bool SaveBookCoverSidecar(const char* book_path);

// 已有 L8 封面时写入旁路；仅在更大时覆盖，供详情 worker 回写。
bool SaveBookCoverSidecarFromL8(const char* book_path, const RasterImage& l8);

// 用原始图片字节写入旁路 .a2i1（JPEG/PNG/A2I1 等），供云推送 coverImageUrl 落盘。
bool SaveBookCoverSidecarFromBytes(const char* book_path, const uint8_t* data, size_t len);

// 删除旁路封面；路径无效或无文件时静默返回。
void DeleteBookCoverSidecar(const char* book_path);

}  // namespace reader
