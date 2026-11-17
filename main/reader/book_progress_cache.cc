#include "book_progress_cache.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace reader {
namespace book_progress_cache {
struct CachedProgress {
    BookSession::ProgressPeek progress; // 单本进度快照
    uint64_t used = 0; // 最近使用次序
};
static std::mutex s_mu;
static std::unordered_map<std::string, CachedProgress> s_map;
static uint64_t s_used = 0; // 仅在 s_mu 内递增

bool TryGet(const char* book_abs_path, BookSession::ProgressPeek& out) {
    out = {};
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    const auto it = s_map.find(book_abs_path);
    if (it == s_map.end()) {
        return false;
    }
    out = it->second.progress;
    it->second.used = ++s_used;
    return true;
}

void Upsert(const char* book_abs_path, const BookSession::ProgressPeek& peek) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_map.size() >= 64 && s_map.find(book_abs_path) == s_map.end()) {
        const auto oldest = std::min_element(s_map.begin(), s_map.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        s_map.erase(oldest);
    }
    s_map[book_abs_path] = {peek, ++s_used};
}

void Erase(const char* book_abs_path) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    s_map.erase(book_abs_path);
}

}  // namespace book_progress_cache
}  // namespace reader
