#include "book_progress_cache.h"

#include "reader_types.h"

#include <mutex>
#include <unordered_map>

#include <esp_log.h>

namespace reader {
namespace book_progress_cache {
namespace {

constexpr const char* TAG = "BookProgCache";

std::mutex s_mu;
std::unordered_map<std::string, BookSession::ProgressPeek> s_map;
bool s_ready = false;
uint32_t s_generation = 0;

}  // namespace

bool IsReady() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_ready;
}

uint32_t Generation() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_generation;
}

void Invalidate() {
    std::lock_guard<std::mutex> lock(s_mu);
    s_map.clear();
    s_ready = false;
    ++s_generation;
    ESP_LOGI(TAG, "invalidated");
}

void ReplaceAll(std::vector<std::pair<std::string, BookSession::ProgressPeek>> items) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_map.clear();
    s_map.reserve(items.size());
    for (auto& it : items) {
        if (it.first.empty()) {
            continue;
        }
        s_map.emplace(std::move(it.first), it.second);
    }
    s_ready = true;
    ++s_generation;
    ESP_LOGI(TAG, "replaced n=%u gen=%u", static_cast<unsigned>(s_map.size()),
             static_cast<unsigned>(s_generation));
}

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
    out = it->second;
    return true;
}

void Upsert(const char* book_abs_path, const BookSession::ProgressPeek& peek) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    s_map[book_abs_path] = peek;
    // 单本更新不 Cleared ready；若尚未 Replace 过，仍允许局部写入供后续合并
    if (!s_ready) {
        s_ready = true;
    }
    ++s_generation;
}

void Erase(const char* book_abs_path) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_map.erase(book_abs_path) > 0) {
        ++s_generation;
        ESP_LOGD(TAG, "erased %s", book_abs_path);
    }
}

size_t Size() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_map.size();
}

size_t FillPeeksForBooks(const std::vector<BookInfo>& books,
                         std::vector<BookSession::ProgressPeek>& out) {
    out.clear();
    out.resize(books.size());
    std::lock_guard<std::mutex> lock(s_mu);
    size_t hit = 0;
    for (size_t i = 0; i < books.size(); ++i) {
        const auto it = s_map.find(books[i].path);
        if (it != s_map.end()) {
            out[i] = it->second;
            ++hit;
        } else {
            out[i] = {};
        }
    }
    return hit;
}

}  // namespace book_progress_cache
}  // namespace reader
