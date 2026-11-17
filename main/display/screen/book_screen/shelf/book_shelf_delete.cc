#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_shelf_ops.h"

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <new>
#include <utility>
#include <esp_heap_caps.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "sd_paths.h"
#include "sd_access.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_font_runtime.h"
#include "standby_screen/standby_screen.h"
#include "power_policy.h"
#include <atomic>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

struct ShelfDeleteWork {
    PowerNeedHold awake{PowerNeed::StandbyInhibit}; // 批次和 UI 收尾期间禁止自动待机
    lv_obj_t* screen = nullptr; // 仅 UI 比较，不由后台访问控件
    std::string directory; // 删除发起时的目录
    std::vector<reader::BookDirEntry> entries; // 已选项快照，不引用页面容器
    std::vector<uint8_t> removed; // 各已选项是否完整删除，工作结束后由 UI 读取
    unsigned failed = 0; // 排除正常取消的失败项数
    bool stopped = false; // 用户取消或介质暂停访问
    int64_t started_us = 0; // 汇总批次耗时
};

static std::unique_ptr<ShelfDeleteWork> s_delete_work;
static std::atomic<bool> s_delete_busy{false};
static std::atomic<bool> s_delete_cancel{false};
static std::atomic<bool> s_delete_finished{false};
static lv_timer_t* s_delete_timer = nullptr;

static lv_obj_t* s_delete_dialog = nullptr; // 书架屏持有的透明弹窗容器
static lv_obj_t* s_delete_message = nullptr; // 弹窗内换行居中的状态文案

static void CloseShelfDeleteDialog() {
    if (!s_delete_dialog) {
        return;
    }
    auto* dialog = s_delete_dialog;
    s_delete_dialog = nullptr;
    s_delete_message = nullptr;
    lv_obj_delete(dialog);
}

static bool ShowShelfDeleteDialog(lv_obj_t* screen) {
    CloseShelfDeleteDialog();
    auto* mask = lv_obj_create(screen);
    if (!mask) {
        return false;
    }
    s_delete_dialog = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    Book_DisableScroll(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        mask,
        [](lv_event_t* event) {
            if (lv_event_get_target(event) == s_delete_dialog) {
                s_delete_dialog = nullptr;
                s_delete_message = nullptr;
            }
        },
        LV_EVENT_DELETE, nullptr);
    auto* card = lv_obj_create(mask);
    if (!card) {
        CloseShelfDeleteDialog();
        return false;
    }
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(80), LV_SIZE_CONTENT);
    ScreenStyleDialogCard(card, UiSx(18));
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* event) { lv_event_stop_bubbling(event); }, LV_EVENT_CLICKED, nullptr);
    s_delete_message = lv_label_create(card);
    if (!s_delete_message) {
        CloseShelfDeleteDialog();
        return false;
    }
    lv_obj_set_width(s_delete_message, lv_pct(100));
    lv_label_set_text(s_delete_message, Lang::Strings::BOOK_DELETING);
    lv_obj_clear_flag(s_delete_message, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_delete_message, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(s_delete_message, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_delete_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_delete_message, LV_LABEL_LONG_WRAP);
    lv_obj_move_foreground(mask);
    return true;
}

bool ShelfDeleteBusy() {
    return s_delete_busy.load();
}

void CancelShelfDelete() {
    s_delete_cancel.store(true);
}

static bool ShelfDeleteShouldStop() {
    return s_delete_cancel.load() || sd_access::IsQuiescing();
}

static bool PathIsUnderBooksRoot(const std::string& path) {
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

static bool DeleteLibraryBookFile(const reader::BookInfo& info, std::string& err_out) {
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

// 递归删除书库下路径；书籍走 DeleteLibraryBookFile 以清进度缓存。
static bool DeleteLibraryPathRecursive(const std::string& path, int depth, std::string& err_out) {
    if (ShelfDeleteShouldStop()) {
        return false;
    }
    if (!PathIsUnderBooksRoot(path)) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    if (depth > 8) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
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
        if (unlink(path.c_str()) == 0 || errno == ENOENT) {
            return true;
        }
        ESP_LOGE(TAG, "library delete failed stage=unlink errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
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
            err_out = Lang::Strings::BOOK_DELETE_FAILED;
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
                    err_out = Lang::Strings::BOOK_DELETE_FAILED;
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
            const bool book =
                ent->d_type != DT_DIR && reader::DetectFormatByPath(ent->d_name) != reader::BookFormat::kUnknown;
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
            err_out = Lang::Strings::BOOK_DELETE_FAILED;
            return false;
        }
    }
    if (ShelfDeleteShouldStop()) {
        return false;
    }
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "library delete failed stage=rmdir errno=%d", errno);
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    ESP_LOGD(TAG, "library folder deleted");
    return true;
}

static void ShelfDeleteWorker(void* arg) {
    auto* work = static_cast<ShelfDeleteWork*>(arg);
    try {
        sd_access::Transaction access;
        if (!access) {
            work->failed = 1;
            ESP_LOGW(TAG, "book batch delete access unavailable");
        } else {
            for (size_t i = 0; i < work->entries.size() && !ShelfDeleteShouldStop(); ++i) {
                auto& entry = work->entries[i];
                std::string error;
                const bool ok = entry.kind == reader::BookDirEntry::Kind::kFolder
                                    ? DeleteLibraryPathRecursive(entry.path, 0, error)
                                    : DeleteLibraryBookFile(entry.book, error);
                if (ok) {
                    work->removed[i] = 1;
                } else if (!ShelfDeleteShouldStop()) {
                    ++work->failed;
                    ESP_LOGW(TAG, "book batch delete item failed kind=%s",
                             entry.kind == reader::BookDirEntry::Kind::kFolder ? "folder" : "book");
                }
                vTaskDelay(1);
            }
        }
    } catch (const std::bad_alloc&) {
        ++work->failed;
        ESP_LOGE(TAG, "book batch delete failed stage=allocate");
    }
    work->stopped = ShelfDeleteShouldStop();
    // 发布完成后不再访问 work；UI 可接管并释放，任务不等待 UI 回调。
    s_delete_finished.store(true);
    vTaskDeleteWithCaps(nullptr);
}

static void ShelfDeleteTick(lv_timer_t*) try {
    if (!s_delete_finished.load()) {
        const auto& shelf = Book_State().shelf;
        if (shelf.list_scr != s_delete_work->screen || lv_screen_active() != s_delete_work->screen ||
            shelf.shelf_dir != s_delete_work->directory || StandbyScreen::IsActive()) {
            CancelShelfDelete();
            CloseShelfDeleteDialog();
        } else if (s_delete_message && shelf.delete_stopping &&
                   std::strcmp(lv_label_get_text(s_delete_message), Lang::Strings::BOOK_DELETE_STOPPING) != 0) {
            lv_label_set_text(s_delete_message, Lang::Strings::BOOK_DELETE_STOPPING);
        }
        return;
    }
    auto work = std::move(s_delete_work);
    CloseShelfDeleteDialog();
    lv_timer_del(s_delete_timer);
    s_delete_timer = nullptr;
    s_delete_busy.store(false);
    unsigned removed = 0;
    for (uint8_t done : work->removed) {
        removed += done != 0;
    }
    ESP_LOGI(TAG, "book batch delete finished selected_removed=%u failed=%u stopped=%d elapsed_ms=%lld", removed,
             work->failed, work->stopped, (esp_timer_get_time() - work->started_us) / 1000);
    reader::InvalidateBookLibraryCache();
    auto& shelf = Book_State().shelf;
    if (shelf.list_scr != work->screen || !shelf.list_scr || shelf.shelf_dir != work->directory ||
        shelf.back_root != BookUiState::NavRoot::kShelf) {
        return;
    }
    for (size_t i = 0; i < work->entries.size(); ++i) {
        if (!work->removed[i]) {
            continue;
        }
        const auto& path = work->entries[i].path;
        shelf.shelf_selected.erase(
            std::remove_if(shelf.shelf_selected.begin(), shelf.shelf_selected.end(),
                           [&](const reader::BookDirEntry& entry) { return entry.path == path; }),
            shelf.shelf_selected.end());
    }
    if (lv_screen_active() != work->screen || StandbyScreen::IsActive()) {
        if (shelf.shelf_selected.empty()) {
            shelf.shelf_multi = false;
        }
        return;
    }
    if (work->failed) {
        ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
    }
    if (shelf.shelf_selected.empty()) {
        ExitShelfMultiMode(false);
    }
    shelf.selected = -1;
    ReloadShelfDirListing();
    RefreshShelfFooterMode();
    RequestRenderBookshelfPage();
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "book batch delete result refresh failed stage=allocate");
    ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
}

void StartShelfDelete() try {
    auto& shelf = Book_State().shelf;
    if (ShelfDeleteBusy() || shelf.directory_loading || shelf.shelf_selected.empty() || StandbyScreen::IsActive()) {
        return;
    }
    auto work = std::make_unique<ShelfDeleteWork>();
    work->screen = shelf.list_scr;
    work->directory = shelf.shelf_dir;
    work->entries = shelf.shelf_selected;
    work->removed.resize(work->entries.size(), 0);
    work->started_us = esp_timer_get_time();
    s_delete_timer = lv_timer_create(ShelfDeleteTick, 100, nullptr);
    if (!s_delete_timer) {
        ESP_LOGE(TAG, "book batch delete timer creation failed");
        ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
        return;
    }
    if (!ShowShelfDeleteDialog(shelf.list_scr)) {
        lv_timer_del(s_delete_timer);
        s_delete_timer = nullptr;
        ESP_LOGE(TAG, "book batch delete dialog creation failed");
        ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
        return;
    }
    CancelListCoverFill();
    CancelShelfDirectory();
    s_delete_cancel.store(false);
    s_delete_finished.store(false);
    shelf.delete_stopping = false;
    s_delete_work = std::move(work);
    s_delete_busy.store(true);
    if (xTaskCreatePinnedToCoreWithCaps(ShelfDeleteWorker, "shelf_delete", 16 * 1024, s_delete_work.get(),
                                        tskIDLE_PRIORITY + 1, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_delete_busy.store(false);
        s_delete_work.reset();
        CloseShelfDeleteDialog();
        lv_timer_del(s_delete_timer);
        s_delete_timer = nullptr;
        ESP_LOGE(TAG, "book batch delete task creation failed");
        ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
        return;
    }
    ESP_LOGI(TAG, "book batch delete started selected=%u", static_cast<unsigned>(shelf.shelf_selected.size()));
    RefreshShelfFooterMode();
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "book batch delete preparation failed stage=allocate");
    ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
}
