#include "book_library.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reader {
namespace {

constexpr const char* TAG = "BookLib";
constexpr int kMaxScanDepth = 8; // 相对根目录的最大嵌套层数

std::mutex s_cache_mu;
std::vector<BookInfo> s_cache;
bool s_cache_valid = false;

bool IsDefaultBooksDir(const char* dir) {
    if (dir == nullptr || dir[0] == '\0') {
        return true;
    }
    return std::strcmp(dir, kDefaultBooksDir) == 0;
}

std::string ToLowerExt(const char* name) {
    std::string s;
    if (name == nullptr) {
        return s;
    }
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) {
        return s;
    }
    s = dot;
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 已知旁路扩展名：不可能是目录，跳过 stat，减少 SD IO
bool IsSidecarExt(const std::string& ext) {
    return ext == ".pos" || ext == ".idx" || ext == ".tmp" || ext == ".cover" ||
           ext == ".bak" || ext == ".part";
}

// 递归收集 dir 下书籍；根目录打不开返回 false，子目录失败仅跳过。
bool AppendBooksFromDir(const char* dir, int depth, std::vector<BookInfo>& out) {
    DIR* d = opendir(dir);
    if (d == nullptr) {
        return false;
    }

    std::string prefix = dir;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }

    size_t entries_seen = 0;
    while (true) {
        errno = 0;
        dirent* ent = readdir(d);
        if (ent == nullptr) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }

        // 让 IDLE 有机会跑，避免深目录扫盘饿死 Task WDT
        if ((++entries_seen & 15u) == 0u) {
            vTaskDelay(1);
        }

        const BookFormat fmt = DetectFormatByPath(ent->d_name);
        const std::string ext = ToLowerExt(ent->d_name);
        if (fmt == BookFormat::kUnknown && IsSidecarExt(ext)) {
            continue;
        }

        const std::string path = prefix + ent->d_name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) {
            ESP_LOGW(TAG, "stat fail: %s errno=%d", path.c_str(), errno);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (depth >= kMaxScanDepth) {
                ESP_LOGW(TAG, "skip deep dir (>%d): %s", kMaxScanDepth, path.c_str());
                continue;
            }
            if (!AppendBooksFromDir(path.c_str(), depth + 1, out)) {
                ESP_LOGW(TAG, "cannot open subdir %s", path.c_str());
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        if (fmt == BookFormat::kUnknown) {
            ESP_LOGD(TAG, "skip: %s", path.c_str());
            continue;
        }

        BookInfo info;
        info.path = path;
        info.title = TitleFromPath(ent->d_name);
        info.format = fmt;
        info.file_size = static_cast<size_t>(st.st_size);
        info.mtime = static_cast<int64_t>(st.st_mtime);
        ESP_LOGD(TAG, "book: %s (%s, %u bytes)", info.path.c_str(), FormatLabel(fmt),
                 static_cast<unsigned>(info.file_size));
        out.push_back(std::move(info));
    }
    closedir(d);
    return true;
}

}  // namespace

void InvalidateBookLibraryCache() {
    std::lock_guard<std::mutex> lock(s_cache_mu);
    s_cache.clear();
    s_cache.shrink_to_fit();
    s_cache_valid = false;
    ESP_LOGI(TAG, "library cache invalidated");
}

void ReplaceBookLibraryCache(std::vector<BookInfo> books) {
    std::lock_guard<std::mutex> lock(s_cache_mu);
    s_cache = std::move(books);
    s_cache_valid = true;
    ESP_LOGI(TAG, "library cache replaced n=%u", static_cast<unsigned>(s_cache.size()));
}

void OfferBookLibraryCache(const std::vector<BookInfo>& books) {
    std::lock_guard<std::mutex> lock(s_cache_mu);
    if (s_cache_valid) {
        ESP_LOGI(TAG, "library cache offer skipped (valid n=%u)",
                 static_cast<unsigned>(s_cache.size()));
        return;
    }
    s_cache = books;
    s_cache_valid = true;
    ESP_LOGI(TAG, "library cache offered n=%u", static_cast<unsigned>(s_cache.size()));
}

bool LoadBookLibrary(const char* dir, std::vector<BookInfo>& out) {
    if (dir == nullptr || dir[0] == '\0') {
        dir = kDefaultBooksDir;
    }

    if (IsDefaultBooksDir(dir)) {
        {
            std::lock_guard<std::mutex> lock(s_cache_mu);
            if (s_cache_valid) {
                out = s_cache;
                ESP_LOGI(TAG, "书库缓存命中(跳过扫盘): %u 本", static_cast<unsigned>(out.size()));
                return true;
            }
        }
    }

    if (!ScanBookLibrary(dir, out)) {
        return false;
    }

    if (IsDefaultBooksDir(dir)) {
        ReplaceBookLibraryCache(out);  // 按值拷贝，保留调用方 out
    }
    return true;
}

BookFormat DetectFormatByPath(const char* path) {
    const std::string ext = ToLowerExt(path);
    if (ext == ".txt") {
        return BookFormat::kTxt;
    }
    // .epub 正常；无 LFN 时 8.3 会把扩展名截成 .epu，一并认作 EPUB
    if (ext == ".epub" || ext == ".epu") {
        return BookFormat::kEpub;
    }
    // .ebook；8.3 可能截成 .ebo
    if (ext == ".ebook" || ext == ".ebo") {
        return BookFormat::kEbook;
    }
    return BookFormat::kUnknown;
}

std::string TitleFromPath(const char* path) {
    if (path == nullptr) {
        return {};
    }
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    std::string title = base;
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos) {
        title = title.substr(0, dot);
    }
    return title;
}

bool ScanBookLibrary(const char* dir, std::vector<BookInfo>& out) {
    out.clear();
    if (dir == nullptr || dir[0] == '\0') {
        dir = kDefaultBooksDir;
    }
    if (!AppendBooksFromDir(dir, 0, out)) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return false;
    }

    // 最新修改时间优先；同时间再按书名
    std::sort(out.begin(), out.end(), [](const BookInfo& a, const BookInfo& b) {
        if (a.mtime != b.mtime) {
            return a.mtime > b.mtime;
        }
        return a.title < b.title;
    });
    ESP_LOGI(TAG, "书库扫描完成: %u 本 dir=%s", static_cast<unsigned>(out.size()), dir);
    return true;
}

bool ScanBookDirListing(const char* dir, std::vector<BookDirEntry>& out) {
    out.clear();
    if (dir == nullptr || dir[0] == '\0') {
        dir = kDefaultBooksDir;
    }
    DIR* d = opendir(dir);
    if (d == nullptr) {
        ESP_LOGW(TAG, "cannot open listing %s", dir);
        return false;
    }

    std::string prefix = dir;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }

    std::vector<BookDirEntry> folders;
    std::vector<BookDirEntry> books;
    size_t entries_seen = 0;
    while (true) {
        errno = 0;
        dirent* ent = readdir(d);
        if (ent == nullptr) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        if ((++entries_seen & 15u) == 0u) {
            vTaskDelay(1);
        }

        const BookFormat fmt = DetectFormatByPath(ent->d_name);
        const std::string ext = ToLowerExt(ent->d_name);
        // 旁路文件直接跳过，避免无意义 stat
        if (fmt == BookFormat::kUnknown && IsSidecarExt(ext)) {
            continue;
        }

        const std::string path = prefix + ent->d_name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            BookDirEntry e;
            e.kind = BookDirEntry::Kind::kFolder;
            e.name = ent->d_name;
            e.path = path;
            folders.push_back(std::move(e));
            continue;
        }

        if (!S_ISREG(st.st_mode) || fmt == BookFormat::kUnknown) {
            // 非书籍类型自动过滤，不进列表
            continue;
        }

        BookDirEntry e;
        e.kind = BookDirEntry::Kind::kBook;
        e.name = ent->d_name;
        e.path = path;
        e.book.path = path;
        e.book.title = TitleFromPath(ent->d_name);
        e.book.format = fmt;
        e.book.file_size = static_cast<size_t>(st.st_size);
        e.book.mtime = static_cast<int64_t>(st.st_mtime);
        books.push_back(std::move(e));
    }
    closedir(d);

    std::sort(folders.begin(), folders.end(),
              [](const BookDirEntry& a, const BookDirEntry& b) { return a.name < b.name; });
    std::sort(books.begin(), books.end(), [](const BookDirEntry& a, const BookDirEntry& b) {
        if (a.book.mtime != b.book.mtime) {
            return a.book.mtime > b.book.mtime;
        }
        return a.name < b.name;
    });

    out.reserve(folders.size() + books.size());
    for (auto& f : folders) {
        out.push_back(std::move(f));
    }
    for (auto& b : books) {
        out.push_back(std::move(b));
    }
    ESP_LOGI(TAG, "dir listing: %u folders + %u books dir=%s",
             static_cast<unsigned>(folders.size()), static_cast<unsigned>(books.size()), dir);
    return true;
}

}  // namespace reader
