#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <esp_timer.h>
#include "assets/lang_config.h"
#include "business/reader/book_library.h"
#include "business/reader/book_delete.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "business/reader/reader.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"

struct ShelfDeleteWork : ScreenUiCompletion {
    lv_obj_t* screen = nullptr; // 仅UI比较，不由后台访问
    std::string directory;
    BookDeleteRequest request;
};
static std::unique_ptr<ShelfDeleteWork> s_delete_work;
static UiPageLifecycle s_delete_lifecycle;

static lv_obj_t* s_delete_dialog = nullptr;   // 书架屏持有的透明弹窗容器
static lv_obj_t* s_delete_message = nullptr;  // 弹窗内换行居中的状态文案

static UiModal s_window_modal;

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
    s_window_modal.Bind(mask, [](void*) { CancelShelfDelete(); });
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    Book_DisableScroll(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
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
    lv_label_set_text(s_delete_message, LangString_BOOK_DELETING);
    lv_obj_clear_flag(s_delete_message, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_delete_message, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(s_delete_message, UiColorForeground(), 0);
    lv_obj_set_style_text_align(s_delete_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_delete_message, LV_LABEL_LONG_WRAP);
    lv_obj_move_foreground(mask);
    return true;
}

bool ShelfDeleteBusy() { return s_delete_work != nullptr; }

void CancelShelfDelete() {
    if (s_delete_work) {
        BookDeleteCancel(s_delete_work->request);
    }
}

static void ShelfDeleteTick(bool completed) try {
    if (!completed) {
        if (!s_delete_work) {
            return;
        }
        const auto& shelf = Book_State().shelf;
        if (shelf.list_scr != s_delete_work->screen || lv_screen_active() != s_delete_work->screen ||
            shelf.shelf_dir != s_delete_work->directory || StandbyPresentation::IsActive()) {
            CancelShelfDelete();
            CloseShelfDeleteDialog();
        } else if (s_delete_message && shelf.delete_stopping &&
                   std::strcmp(lv_label_get_text(s_delete_message), LangString_BOOK_DELETE_STOPPING) != 0) {
            lv_label_set_text(s_delete_message, LangString_BOOK_DELETE_STOPPING);
        }
        return;
    }
    auto work = std::move(s_delete_work);
    CloseShelfDeleteDialog();
    s_delete_lifecycle.Unbind();
    unsigned removed = 0;
    for (uint8_t done : work->request->removed) {
        removed += done != 0;
    }
    ESP_LOGI(TAG, "book batch delete finished selected_removed=%u failed=%u stopped=%d elapsed_ms=%lld", removed,
             work->request->failed, work->request->stopped, (esp_timer_get_time() - work->request->started_us) / 1000);
    auto& shelf = Book_State().shelf;
    if (shelf.list_scr != work->screen || !shelf.list_scr || shelf.shelf_dir != work->directory ||
        shelf.back_root != BookUiState::NavRoot::kShelf) {
        return;
    }
    for (size_t i = 0; i < work->request->entries.size(); ++i) {
        if (!work->request->removed[i]) {
            continue;
        }
        const auto& path = work->request->entries[i].path;
        shelf.shelf_selected.erase(std::remove_if(shelf.shelf_selected.begin(), shelf.shelf_selected.end(),
                                                  [&](const BookDirEntry& entry) { return entry.path == path; }),
                                   shelf.shelf_selected.end());
    }
    if (lv_screen_active() != work->screen || StandbyPresentation::IsActive()) {
        if (shelf.shelf_selected.empty()) {
            shelf.shelf_multi = false;
        }
        return;
    }
    if (work->request->failed) {
        ScreenShowToast(LangString_BOOK_DELETE_FAILED);
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
    ScreenShowToast(LangString_BOOK_DELETE_FAILED);
}

void StartShelfDelete() try {
    auto& shelf = Book_State().shelf;
    if (ShelfDeleteBusy() || shelf.directory_loading || shelf.shelf_selected.empty() || StandbyPresentation::IsActive()) {
        return;
    }
    auto work = std::make_unique<ShelfDeleteWork>();
    work->screen = shelf.list_scr;
    work->directory = shelf.shelf_dir;
    if (!ShowShelfDeleteDialog(shelf.list_scr)) {
        ESP_LOGE(TAG, "book batch delete dialog creation failed");
        ScreenShowToast(LangString_BOOK_DELETE_FAILED);
        return;
    }
    CancelListCoverFill();
    CancelShelfDirectory();
    shelf.delete_stopping = false;
    s_delete_work = std::move(work);
    auto* context = s_delete_work.get();
    context->apply = [](ScreenUiCompletion*) { ShelfDeleteTick(true); };
    context->request = BookDeleteStart(shelf.shelf_selected, [context]() { ScreenCompleteWork(context); });
    if (!context->request) {
        s_delete_work.reset();
        CloseShelfDeleteDialog();
        ScreenShowToast(LangString_BOOK_DELETE_FAILED);
        return;
    }
    s_delete_lifecycle.Bind(shelf.list_scr, {[]() { ShelfDeleteTick(false); }, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications) {
            CancelShelfDelete();
            CloseShelfDeleteDialog();
        }
    }, []() { CancelShelfDelete(); CloseShelfDeleteDialog(); }, 100});
    ESP_LOGI(TAG, "book batch delete started selected=%u", static_cast<unsigned>(shelf.shelf_selected.size()));
    RefreshShelfFooterMode();
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "book batch delete preparation failed stage=allocate");
    ScreenShowToast(LangString_BOOK_DELETE_FAILED);
}
