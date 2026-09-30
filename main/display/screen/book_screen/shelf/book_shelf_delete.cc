#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_shelf_ops.h"

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "reader/book_cover_sidecar.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
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
        return true; // 已不存在
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
        unlink(path.c_str());
        return true;
    }
    if (!S_ISDIR(st.st_mode)) {
        return true;
    }
    DIR* d = opendir(path.c_str());
    if (d == nullptr) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    std::string prefix = path;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    while (true) {
        errno = 0;
        dirent* ent = readdir(d);
        if (ent == nullptr) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        const std::string child = prefix + ent->d_name;
        if (!DeleteLibraryPathRecursive(child, depth + 1, err_out)) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    ESP_LOGI(TAG, "deleted library folder %s", path.c_str());
    return true;
}

bool DeleteLibraryFolder(const std::string& dir, std::string& err_out) {
    err_out.clear();
    if (!DeleteLibraryPathRecursive(dir, 0, err_out)) {
        return false;
    }
    EraseBooksUnderDirPrefix(dir);
    return true;
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
    // 先 Peek/缓存 再删 .pos：供首页聚合扣减（删后无法再读）
    reader::BookSession::ProgressPeek peek;
    if (!reader::book_progress_cache::TryGet(info.path.c_str(), peek)) {
        peek = reader::BookSession::PeekProgress(info.path.c_str());
    }
    if (unlink(info.path.c_str()) != 0) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    unlink((info.path + ".tmp").c_str());
    unlink((info.path + ".pos").c_str());      // 进度+终身累计+当日阅读秒
    unlink((info.path + ".pos.tmp").c_str());  // 原子写残留
    reader::DeleteBookCoverSidecar(info.path.c_str());
    // TXT 分页索引缓存：同目录 <name>.txt.idx
    if (info.format == reader::BookFormat::kTxt) {
        unlink((info.path + ".idx").c_str());
    }
    ESP_LOGI(TAG, "deleted library book %s", info.path.c_str());
    reader::book_progress_cache::Erase(info.path.c_str());
    reader::book_home_snapshot::RemoveBook(info.path.c_str());
    reader::book_home_snapshot::SubtractAggregateContribution(
        peek.reading_seconds, peek.progress_x10 >= 1000);
    return true;
}

