
#include <atomic>
#include <cerrno>
#include <new>
#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <unistd.h>
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/book/book_screen.h"
#include "assets/lang_config.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "boards/common/storage/sd_access.h"
#include "business/reader/book_home_snapshot.h"
#include "business/reader/book_library.h"
#include "business/reader/book_progress_cache.h"
#include "business/reader/reader.h"
#include "infra/storage/settings.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/book_vk.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_orient.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/apps/book/shelf/book_home.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/home/home_screen.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

ScreenPaintCoalesce s_reader_paint{};
ScreenPaintCoalesce s_library_paint{};
ScreenPaintCoalesce s_shelf_check_paint{};
ScreenPaintCoalesce s_toc_paint{};
ScreenPaintCoalesce s_settings_sheet_paint{};  // 设置卡：连点只记状态，抬起后一次刷控件

static BookUiState s_ui_state;

BookUiState& Book_State() {
    return s_ui_state;
}

static std::atomic<unsigned> s_recent_generation{0};

struct RecentProgressWork : ScreenUiCompletion {
    unsigned generation = 0;         // 阅读首页代次
    lv_obj_t* screen = nullptr;      // 仅在 UI 回调中比较，不由后台访问
    std::vector<std::string> paths;  // 最多两本可见最近阅读
};

static void LoadRecentProgress(void* arg) {
    std::unique_ptr<RecentProgressWork> owner(static_cast<RecentProgressWork*>(arg));
    auto* work = owner.get();
    try {
        SdAccessTransaction transaction;
        if (transaction) {
            for (const auto& path : work->paths) {
                if (work->generation != s_recent_generation.load() || SdAccessIsQuiescing()) {
                    break;
                }
                BookProgressCacheUpsert(path.c_str(), BookSession::PeekProgress(path.c_str()));
            }
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGW("BookHome", "operation=recent_progress stage=load result=no_memory");
    }
    work->apply = [](ScreenUiCompletion* completion) {
        std::unique_ptr<RecentProgressWork> owner(static_cast<RecentProgressWork*>(completion));
        if (owner->generation == s_recent_generation.load() && Book_State().shelf.list_scr == owner->screen &&
            lv_screen_active() == owner->screen) {
            RenderReadingHome();
        }
    };
    ScreenCompleteWork(owner.release());
}

static std::string s_boot_resume_path;

static lv_obj_t* CreateBookHome(bool continue_latest, const std::string& resume_path = {}) {
    ++s_recent_generation;
    auto& st = Book_State();
    CancelShelfDirectory();
    ReleaseReaderSessionForLeave();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.multi_bar = nullptr;
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    CancelListCoverFill();
    st.shelf.recent_covers.clear();
    st.shelf.shelf_covers.clear();

    ScreenSetIsHome(false);

    if (continue_latest && !ReaderWorkersBusy() && !st.reader.deferred_cleanup &&
        SdCardManager::GetInstance().IsMounted()) {
        const std::string latest = resume_path.empty() ? BookHomeSnapshotLoadLatestAbsPath() : resume_path;
        if (!latest.empty()) {
            BookInfo info;
            info.path = latest;
            info.title = TitleFromPath(latest.c_str());
            info.format = DetectFormatByPath(latest.c_str());
            st.shelf.books = {info};
            for (size_t i = 0; i < st.shelf.books.size(); ++i) {
                if (st.shelf.books[i].path == latest) {
                    st.shelf.selected = static_cast<int>(i);
                    return CreateReaderScreen(st.shelf.books[i]);
                }
            }
        }
    }

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    st.shelf.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.shelf.list_scr == tgt) {
                CancelListCoverFill();
                s.shelf.list_scr = nullptr;
                s.shelf.list_body = nullptr;
                s.shelf.list_footer = nullptr;
                s.shelf.multi_bar = nullptr;
                s.shelf.shelf_multi = false;
                s.shelf.shelf_selected.clear();
                s.shelf.recent_covers.clear();
                s.shelf.shelf_covers.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_label = status.status_label;
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, kListPad, 0);
    lv_obj_set_style_pad_bottom(body, kListPad + kHomeBottomPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kHomeGap, 0);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, Book_ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, 0, 0);
    Book_DisableScroll(list_body);
    st.shelf.list_body = list_body;
    st.shelf.list_footer = nullptr;
    st.shelf.multi_bar = nullptr;

    if (!SdCardManager::GetInstance().IsMounted()) {
        Book_ShowMessage(list_body, LangString_BOOK_NO_SD, Book_ListFont());
        BookBindPageLifecycle(scr);
        VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
        return scr;
    }

    st.shelf.books.clear();
    // 首页只恢复固定数量的最近阅读，不枚举书库。
    std::vector<std::string> recent;
    BookHomeSnapshotLoadRecentAbsPaths(recent);
    for (const auto& path : recent) {
        BookInfo info;
        info.path = path;
        info.title = TitleFromPath(path.c_str());
        info.format = DetectFormatByPath(path.c_str());
        st.shelf.books.push_back(std::move(info));
        if (st.shelf.books.size() >= kBookHomeSnapshotRecentDisplayMax) {
            break;
        }
    }
    RenderReadingHome();
    if (!st.shelf.books.empty()) {
        auto* work = new (std::nothrow) RecentProgressWork;
        if (work) {
            work->screen = scr;
            work->generation = s_recent_generation.load();
            for (const auto& info : st.shelf.books) {
                work->paths.push_back(info.path);
            }
            if (!TaskExecutor::Schedule([work]() { LoadRecentProgress(work); }, TaskDomain::Storage)) {
                delete work;
                ESP_LOGE("BookHome", "recent progress task creation failed");
            }
        } else {
            ESP_LOGE("BookHome", "recent progress allocation failed");
        }
    }

    // factory=Create：进百问后返回阅读首页
    BookBindPageLifecycle(scr);
    VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
    return scr;
}

lv_obj_t* BookScreen::Create() {
    return CreateBookHome(false);
}

lv_obj_t* BookScreen::CreateLatest() {
    return CreateBookHome(true);
}

bool BookScreen::LoadBootResume() {
    Settings settings("reader_boot", true);
    s_boot_resume_path = settings.GetString("book");
    if (s_boot_resume_path.empty()) {
        return false;
    }
    settings.EraseKey("book");
    if (!settings.Commit() || !settings.GetString("book").empty()) {
        ESP_LOGE("BookBoot", "boot resume consumption failed; opening home");
        s_boot_resume_path.clear();
        return false;
    }
    return true;
}

lv_obj_t* BookScreen::CreateBootResume() {
    std::string path = std::move(s_boot_resume_path);
    s_boot_resume_path.clear();
    if (path.empty()) {
        ESP_LOGD("BookBoot", "resume skipped reason=no_record");
        return HomeScreen::Create();
    }
    SdAccessTransaction transaction;
    const char* reason = nullptr;
    int file_error = 0;
    if (!SdCardManager::GetInstance().IsMounted()) {
        reason = "sd_unmounted";
    } else if (!transaction) {
        reason = "transaction_rejected";
    } else if (access(path.c_str(), R_OK) != 0) {
        file_error = errno;
        reason = file_error == ENOENT ? "file_missing" : "file_access";
    }
    if (reason) {
        ESP_LOGW("BookBoot", "resume unavailable reason=%s errno=%d; opening home", reason, file_error);
        return HomeScreen::Create();
    }
    ESP_LOGI("BookBoot", "restoring reader");
    return CreateBookHome(true, path);
}

bool BookScreen::PreparePowerOff() {
    static std::atomic<int> saved{0};  // 0 未投递，1 写入中，2 已完成
    auto& st = Book_State();
    if (!st.reader.powering_off) {
        st.reader.powering_off = true;
        OnEnterStandby();
    }
    if (ReaderWorkersBusy()) {
        return false;
    }
    if (saved.load(std::memory_order_acquire) == 2) {
        return true;
    }
    if (saved.load(std::memory_order_acquire) == 1) {
        return false;
    }
    std::string path;
    SdAccessPermit permit;
    if (st.reader.read_scr && st.reader.read_scr == lv_screen_active() && !st.reader.deferred_cleanup &&
        st.reader.session && st.reader.session->IsOpen()) {
        EndReadingTimeTracking();
        if (!st.reader.session->SaveProgress()) {
            ESP_LOGE("BookBoot", "shutdown progress enqueue failed; retaining last saved position");
        }
        path = st.reader.session->Info().path;
    }
    // Storage 串行队列先完成此前提交的 .pos 写入，再持久化具体书籍的恢复记录。
    saved.store(1, std::memory_order_release);
    if (!TaskExecutor::Schedule(
            [path = std::move(path)] {
                Settings settings("reader_boot", true);
                const bool written = path.empty() ? settings.EraseKey("book") : settings.SetString("book", path);
                const bool ok = written && settings.Commit() && settings.GetString("book") == path;
                if (!ok) {
                    ESP_LOGE("BookBoot", "shutdown resume record failed");
                } else {
                    ESP_LOGI("BookBoot", "shutdown resume saved reader=%d", !path.empty());
                }
                saved.store(2, std::memory_order_release);
            },
            TaskDomain::Storage)) {
        saved.store(2, std::memory_order_release);
        ESP_LOGE("BookBoot", "shutdown resume scheduling failed");
    }
    return false;
}
