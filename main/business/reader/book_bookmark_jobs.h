#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include "book_bookmarks.h"

struct BookBookmarkResult {
    std::atomic<bool> completed{false};
    BookBookmarkStore store;
    std::string error;
    bool ok = false;
};
using BookBookmarkRequest = std::shared_ptr<BookBookmarkResult>;

/** @brief 在内部RAM存储通道保存独立书签快照，空remove表示添加；回调在后台完成后调用。 */
BookBookmarkRequest BookBookmarkSubmit(const BookBookmarkStore& store, BookBookmark entry,
                                       std::vector<uint32_t> remove,
                                       std::function<void()> completed);
