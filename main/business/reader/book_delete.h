#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "business/reader/book_library.h"

struct BookDeleteResult {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    std::vector<BookDirEntry> entries;
    std::vector<uint8_t> removed;
    unsigned failed = 0;
    bool stopped = false;
    int64_t started_us = 0;
};
using BookDeleteRequest = std::shared_ptr<BookDeleteResult>;

/** @brief 接纳书籍或目录批量删除，保留部分成功，独立提交目录缓存失效。 */
BookDeleteRequest BookDeleteStart(const std::vector<BookDirEntry>& entries, std::function<void()> completed);

/** @brief 在当前文件操作结束后停止，已完成的删除不会回滚。 */
void BookDeleteCancel(const BookDeleteRequest& request);
