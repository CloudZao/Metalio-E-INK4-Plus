#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "business/reader/reader.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"
#include "book_delete.h"

static constexpr const char* TAG = "BookDelete";
static std::mutex s_mutex;
static std::weak_ptr<BookDeleteResult> s_request;
static thread_local BookDeleteResult* s_active = nullptr;
static bool ShelfDeleteShouldStop() {
    return !s_active || s_active->cancelled.load() || SdAccessIsQuiescing();
}

static bool PathIsUnderBooksRoot(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        return false;
    }
    const std::string root = kDefaultBooksDir;
    if (path == root) {
        return false;  // 禁止删书库根
    }
    const std::string prefix = root + "/";
    if (path.rfind(prefix, 0) != 0 || path.size() >= SD_FILE_PATH_CAPACITY) {
        return false;
    }
    for (size_t start = prefix.size(); start < path.size();) {
        const size_t end = path.find('/', start);
        const auto component = path.substr(start, end == std::string::npos ? end : end - start);
        if (!SdIsValidFileName(component.c_str())) {
            return false;
        }
        if (end == std::string::npos) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

static bool DeleteLibraryBookFile(const BookInfo& info, std::string& err_out) {
    err_out.clear();
    if (!PathIsUnderBooksRoot(info.path)) {
        err_out = LangString_BOOK_PATH_INVALID;
        return false;
    }
    const std::string books_prefix = std::string(SD_PATH_BOOKS) + "/";
    if (info.path.rfind(books_prefix, 0) != 0) {
        err_out = LangString_BOOK_PATH_INVALID;
        return false;
    }
    if (!BookSession::DeleteFile(info.path.c_str())) {
        err_out = LangString_BOOK_DELETE_FAILED;
        return false;
    }
    return true;
}

// 递归删除书库下路径；书籍走 DeleteLibraryBookFile 以清进度缓存。
static bool DeleteLibraryPathRecursive(const std::string& path, int depth, std::string& err_out) {
    if (ShelfDeleteShouldStop()) {
        return false;
    }
    if (!PathIsUnderBooksRoot(path)) {
        err_out = LangString_BOOK_PATH_INVALID;
        return false;
    }
    if (depth > 8) {
        err_out = LangString_BOOK_DELETE_FAILED;
        return false;
    }
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        ESP_LOGE(TAG, "library delete failed stage=stat errno=%d", errno);
        err_out = LangString_BOOK_DELETE_FAILED;
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        const BookFormat fmt = DetectFormatByPath(path.c_str());
        if (fmt != BookFormat::kUnknown) {
            BookInfo info;
            info.path = path;
            info.title = TitleFromPath(path.c_str());
            info.format = fmt;
            info.file_size = static_cast<size_t>(st.st_size);
            info.mtime = static_cast<int64_t>(st.st_mtime);
            return DeleteLibraryBookFile(info, err_out);
        }
        if (unlink(path.c_str()) == 0 || errno == ENOENT) {
            return true;
        }
        ESP_LOGE(TAG, "library delete failed stage=unlink errno=%d", errno);
        err_out = LangString_BOOK_DELETE_FAILED;
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return true;
    }
    // 先处理当前目录的书籍，再处理子目录及辅助文件，避免先删进度导致统计基线丢失。
    // 两遍顺序枚举不保存整目录路径；取消时只需关闭当前递归链的句柄。
    for (int pass = 0; pass < 2; ++pass) {
        if (ShelfDeleteShouldStop()) {
            return false;
        }
        DIR* d = opendir(path.c_str());
        if (!d) {
            ESP_LOGE(TAG, "library delete failed stage=opendir errno=%d", errno);
            err_out = LangString_BOOK_DELETE_FAILED;
            return false;
        }
        std::unique_ptr<DIR, int (*)(DIR*)> directory(d, closedir);
        unsigned visited = 0;
        while (!ShelfDeleteShouldStop()) {
            errno = 0;
            dirent* ent = readdir(d);
            if (!ent) {
                if (errno) {
                    ESP_LOGE(TAG, "library delete failed stage=readdir errno=%d", errno);
                    err_out = LangString_BOOK_DELETE_FAILED;
                    return false;
                }
                break;
            }
            if ((++visited & 15u) == 0) {
                vTaskDelay(1);
            }
            if (!std::strcmp(ent->d_name, ".") || !std::strcmp(ent->d_name, "..")) {
                continue;
            }
            const bool book = ent->d_type != DT_DIR && DetectFormatByPath(ent->d_name) != BookFormat::kUnknown;
            if (pass == 0 && !book) {
                continue;
            }
            if (ShelfDeleteShouldStop()) {
                return false;
            }
            if (!DeleteLibraryPathRecursive(path + "/" + ent->d_name, depth + 1, err_out)) {
                return false;
            }
            vTaskDelay(1);
        }
        const int closed = closedir(directory.release());
        if (closed) {
            ESP_LOGE(TAG, "library delete failed stage=closedir errno=%d", errno);
            err_out = LangString_BOOK_DELETE_FAILED;
            return false;
        }
    }
    if (ShelfDeleteShouldStop()) {
        return false;
    }
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "library delete failed stage=rmdir errno=%d", errno);
        err_out = LangString_BOOK_DELETE_FAILED;
        return false;
    }
    ESP_LOGD(TAG, "library folder deleted");
    return true;
}

static void RunBookDelete(BookDeleteResult* work) {
    try {
        SdAccessTransaction access;
        if (!access) {
            work->failed = 1;
            ESP_LOGW(TAG, "book batch delete access unavailable");
        } else {
            for (size_t i = 0; i < work->entries.size() && !ShelfDeleteShouldStop(); ++i) {
                auto& entry = work->entries[i];
                std::string error;
                const bool ok = entry.kind == BookDirEntry::Kind::kFolder
                                    ? DeleteLibraryPathRecursive(entry.path, 0, error)
                                    : DeleteLibraryBookFile(entry.book, error);
                if (ok) {
                    work->removed[i] = 1;
                } else if (!ShelfDeleteShouldStop()) {
                    ++work->failed;
                    ESP_LOGW(TAG, "book batch delete item failed kind=%s",
                             entry.kind == BookDirEntry::Kind::kFolder ? "folder" : "book");
                }
                vTaskDelay(1);
            }
        }
    } catch (const std::bad_alloc&) {
        ++work->failed;
        ESP_LOGE(TAG, "book batch delete failed stage=allocate");
    }
    work->stopped = ShelfDeleteShouldStop();
    InvalidateBookLibraryCache();
}


BookDeleteRequest BookDeleteStart(const std::vector<BookDirEntry>& entries, std::function<void()> completed) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto previous = s_request.lock();
    if (entries.empty() || (previous && !previous->completed.load(std::memory_order_acquire))) {
        return {};
    }
    auto request = std::make_shared<BookDeleteResult>();
    request->entries = entries;
    request->removed.resize(entries.size(), 0);
    request->started_us = esp_timer_get_time();
    if (!TaskExecutor::Schedule([request, completed = std::move(completed)]() {
        {
            PowerNeedHold awake(PowerNeed::StandbyInhibit);
            s_active = request.get();
            RunBookDelete(request.get());
            s_active = nullptr;
        }
        request->completed.store(true, std::memory_order_release);
        if (completed) {
            completed();
        }
    }, TaskDomain::Image)) {
        return {};
    }
    s_request = request;
    return request;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=delete stage=submit result=no_memory");
    return {};
}

void BookDeleteCancel(const BookDeleteRequest& request) {
    if (request) {
        request->cancelled.store(true);
    }
}
