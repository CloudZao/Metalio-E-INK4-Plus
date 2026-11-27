#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include "book_progress_cache.h"

struct BookCachedProgress {
    BookSession::ProgressPeek progress;  // 单本进度快照
    uint64_t used = 0;                   // 最近使用次序
};

static std::mutex book_progress_cache_s_mu;
static std::unordered_map<std::string, BookCachedProgress> book_progress_cache_s_map;
static uint64_t book_progress_cache_s_used = 0;  // 仅在 s_mu 内递增

bool BookProgressCacheTryGet(const char* book_abs_path, BookSession::ProgressPeek& out) {
    out = {};
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return false;
    }
    std::lock_guard<std::mutex> lock(book_progress_cache_s_mu);
    const auto it = book_progress_cache_s_map.find(book_abs_path);
    if (it == book_progress_cache_s_map.end()) {
        return false;
    }
    out = it->second.progress;
    it->second.used = ++book_progress_cache_s_used;
    return true;
}

void BookProgressCacheUpsert(const char* book_abs_path, const BookSession::ProgressPeek& peek) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(book_progress_cache_s_mu);
    if (book_progress_cache_s_map.size() >= 64 &&
        book_progress_cache_s_map.find(book_abs_path) == book_progress_cache_s_map.end()) {
        const auto oldest =
            std::min_element(book_progress_cache_s_map.begin(), book_progress_cache_s_map.end(),
                             [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        book_progress_cache_s_map.erase(oldest);
    }
    book_progress_cache_s_map[book_abs_path] = {peek, ++book_progress_cache_s_used};
}

void BookProgressCacheErase(const char* book_abs_path) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(book_progress_cache_s_mu);
    for (auto it = book_progress_cache_s_map.begin(); it != book_progress_cache_s_map.end(); ++it) {
        if (it->first == book_abs_path) {
            book_progress_cache_s_map.erase(it);
            return;
        }
    }
}
