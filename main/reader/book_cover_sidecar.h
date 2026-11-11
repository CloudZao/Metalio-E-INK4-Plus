#pragma once

#include <cstddef>

#include "reader_types.h"

namespace reader {

// 书库旁路封面：temp/books/cache 下按书籍相对路径标识保存 .a2l8（兼容旧 .a2i1）。
// UI 只读旁路；无旁路或尺寸不够时后台抽内嵌并按权威档回写。

/** @brief 生成集中封面缓存路径，兼容 A2L8 和 A2I1；失败返回 false。 */
bool BookCoverSidecarPath(const char* book_path, char* out, size_t out_sz);

/** @brief 查询封面缓存尺寸是否满足显示区域要求。 */
bool BookCoverSidecarAdequate(const char* book_path, int need_w, int need_h);

/** @brief 从 EPUB/.ebook 提取并原子保存封面，已有 A2I1 缓存保持原编码；无封面或失败返回 false。 */
bool SaveBookCoverSidecar(const char* book_path);

/** @brief 保存已解码的 L8 封面，仅在尺寸更大时覆盖已有缓存。 */
bool SaveBookCoverSidecarFromL8(const char* book_path, const RasterImage& l8);

/** @brief 删除封面缓存；路径无效或无文件时直接返回。 */
void DeleteBookCoverSidecar(const char* book_path);

}  // namespace reader
