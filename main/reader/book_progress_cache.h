#pragma once
#include "book_session.h"

namespace reader {
namespace book_progress_cache {
/** @brief 查询按需读取的进度缓存，未命中不访问 SD */
bool TryGet(const char* book_abs_path, BookSession::ProgressPeek& out);
/** @brief 更新单本缓存；最多保留 64 本，超限时淘汰旧项 */
void Upsert(const char* book_abs_path, const BookSession::ProgressPeek& peek);
/** @brief 删除单本缓存 */
void Erase(const char* book_abs_path);
}
}
