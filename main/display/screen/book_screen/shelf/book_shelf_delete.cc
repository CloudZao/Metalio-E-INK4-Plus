#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_shelf_ops.h"

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "sd_paths.h"

bool PathIsUnderBooksRoot(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const std::string root = reader::kDefaultBooksDir;
    if (path == root) {
        return false; // 禁止删书库根
    }
    const std::string prefix = root + "/";
    return path.rfind(prefix, 0) == 0;
}

void EraseBooksUnderDirPrefix(const std::string& dir) {
    auto& st = Book_State();
    std::string prefix = dir;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    for (int i = static_cast<int>(st.shelf.books.size()) - 1; i >= 0; --i) {
        const std::string& p = st.shelf.books[static_cast<size_t>(i)].path;
        if (p == dir || p.rfind(prefix, 0) == 0) {
            if (st.shelf.selected == i) {
                st.shelf.selected = -1;
            } else if (st.shelf.selected > i) {
                --st.shelf.selected;
            }
            st.shelf.books.erase(st.shelf.books.begin() + i);
        }
    }
}

// 递归删除书库下路径；书籍走 DeleteLibraryBookFile 以清进度缓存。
bool DeleteLibraryPathRecursive(const std::string& path, int depth, std::string& err_out) {
    if (!PathIsUnderBooksRoot(path)) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    if (depth > 8) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        ESP_LOGE(TAG, "library delete failed stage=stat errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        const reader::BookFormat fmt = reader::DetectFormatByPath(path.c_str());
        if (fmt != reader::BookFormat::kUnknown) {
            reader::BookInfo info;
            info.path = path;
            info.title = reader::TitleFromPath(path.c_str());
            info.format = fmt;
            info.file_size = static_cast<size_t>(st.st_size);
            info.mtime = static_cast<int64_t>(st.st_mtime);
            return DeleteLibraryBookFile(info, err_out);
        }
        if (unlink(path.c_str()) == 0 || errno == ENOENT) return true;
        ESP_LOGE(TAG, "library delete failed stage=unlink errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return true;
    }
    DIR* d = opendir(path.c_str());
    if (d == nullptr) {
        ESP_LOGE(TAG, "library delete failed stage=opendir errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> directory(d, closedir);
    std::string prefix = path;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    std::vector<std::string> children;
    int read_error = 0;
    while (true) {
        errno = 0;
        dirent* ent = readdir(d);
        if (ent == nullptr) {
            read_error = errno;
            break;
        }
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        children.push_back(prefix + ent->d_name);
    }
    const bool closed = closedir(directory.release()) == 0;
    if (read_error || !closed) {
        ESP_LOGE(TAG, "library delete failed stage=list errno=%d", read_error ? read_error : errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    // 先删书籍，读取进度并扣减统计后再处理同目录的旧辅助文件。
    std::stable_sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
        return reader::DetectFormatByPath(a.c_str()) != reader::BookFormat::kUnknown &&
               reader::DetectFormatByPath(b.c_str()) == reader::BookFormat::kUnknown;
    });
    for (const auto& child : children)
        if (!DeleteLibraryPathRecursive(child, depth + 1, err_out)) return false;
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "library delete failed stage=rmdir errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    ESP_LOGI(TAG, "deleted library folder %s", path.c_str());
    return true;
}

bool DeleteLibraryFolder(const std::string& dir, std::string& err_out) try {
    err_out.clear();
    if (!DeleteLibraryPathRecursive(dir, 0, err_out)) {
        return false;
    }
    EraseBooksUnderDirPrefix(dir);
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "library delete failed stage=allocate");
    err_out = Lang::Strings::BOOK_DELETE_FAILED;
    return false;
}

bool DeleteLibraryBookFile(const reader::BookInfo& info, std::string& err_out) {
    err_out.clear();
    if (info.path.empty()) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    const std::string books_prefix = std::string(SD_PATH_BOOKS) + "/";
    if (info.path.rfind(books_prefix, 0) != 0) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    if (!reader::BookSession::DeleteFile(info.path.c_str())) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    return true;
}
