#pragma once

#include <cstddef>

#include "reader_types.h"

namespace reader {

// 书库旁路封面：同书文件 stem，新扩展名 .a2l8（兼容旧 .a2i1）（例：元尊.ebook -> 元尊.a2l8）。
// UI 只读旁路；无旁路或尺寸不够时后台抽内嵌并按权威档回写。

// 由电子书绝对路径生成旁路缓存（新 .a2l8，旧 .a2i1） 路径；失败返回 false。
bool BookCoverSidecarPath(const char* book_path, char* out, size_t out_sz);

// 旁路是否足够覆盖 need 框；不够时走重抽升级。
bool BookCoverSidecarAdequate(const char* book_path, int need_w, int need_h);

// 从 EPUB/.ebook 提取封面并原子写入新灰阶旁路 A2L8（已有旧缓存不迁移）；无封面或失败返回 false。
bool SaveBookCoverSidecar(const char* book_path);

// 已有 L8 封面时写入旁路；仅在更大时覆盖，供详情 worker 回写。
bool SaveBookCoverSidecarFromL8(const char* book_path, const RasterImage& l8);

// 删除旁路封面；路径无效或无文件时静默返回。
void DeleteBookCoverSidecar(const char* book_path);

}  // namespace reader
