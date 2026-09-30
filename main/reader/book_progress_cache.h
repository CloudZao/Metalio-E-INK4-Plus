#pragma once

#include "book_session.h"
#include "reader_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace reader {
namespace book_progress_cache {

/**
 * 进程内「每书进度/时长」缓存（与书库列表缓存正交）。
 *
 * - 由 book_library_warmup 开机扫盘 Peek 后 Replace
 * - sync / 首页等按需 TryGet，禁止再为全库去 SD 开 .pos
 * - 写进度 Upsert、删书 Erase；仅内存，无线程外 NVS
 */

bool IsReady();

/** Replace / Erase / Upsert 递增，供观察者检测变更 */
uint32_t Generation();

void Invalidate();

/** 整表替换并置 ready；items 可为空（空书库） */
void ReplaceAll(std::vector<std::pair<std::string, BookSession::ProgressPeek>> items);

bool TryGet(const char* book_abs_path, BookSession::ProgressPeek& out);

void Upsert(const char* book_abs_path, const BookSession::ProgressPeek& peek);

void Erase(const char* book_abs_path);

size_t Size();

/**
 * 按 books 顺序取出 peek（缺省则零值）；供 sync 组包，不再碰 SD。
 * @return 成功匹配条数（含缺省填零）
 */
size_t FillPeeksForBooks(const std::vector<BookInfo>& books,
                         std::vector<BookSession::ProgressPeek>& out);

}  // namespace book_progress_cache
}  // namespace reader
