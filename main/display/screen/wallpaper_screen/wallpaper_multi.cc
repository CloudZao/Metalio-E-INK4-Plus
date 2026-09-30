#include "wallpaper_screen/wallpaper_multi.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_ops.h"
#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "haptic_feedback.h"

void SyncWpSelectedSize() {
    if (Wallpaper_State().list.selected.size() != Wallpaper_State().list.files.size()) {
        Wallpaper_State().list.selected.assign(Wallpaper_State().list.files.size(), 0);
    }
}

int WpSelectedCount() {
    SyncWpSelectedSize();
    int n = 0;
    for (uint8_t v : Wallpaper_State().list.selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool WpItemSelected(int idx) {
    SyncWpSelectedSize();
    return idx >= 0 && idx < static_cast<int>(Wallpaper_State().list.selected.size()) &&
           Wallpaper_State().list.selected[static_cast<size_t>(idx)] != 0;
}

void ToggleWpItemSelected(int idx) {
    SyncWpSelectedSize();
    if (idx < 0 || idx >= static_cast<int>(Wallpaper_State().list.selected.size())) {
        return;
    }
    Wallpaper_State().list.selected[static_cast<size_t>(idx)] = Wallpaper_State().list.selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshWpFooterMode() {
    if (Wallpaper_State().mode != UiMode::kList) {
        if (Wallpaper_State().ui.multi_bar != nullptr) {
            lv_obj_add_flag(Wallpaper_State().ui.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (Wallpaper_State().ui.page_lbl != nullptr) {
            lv_obj_add_flag(Wallpaper_State().ui.page_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (Wallpaper_State().list.multi) {
        if (Wallpaper_State().ui.multi_bar != nullptr) {
            lv_obj_set_width(Wallpaper_State().ui.multi_bar, lv_pct(100));
            lv_obj_align(Wallpaper_State().ui.multi_bar, LV_ALIGN_CENTER, 0, -3);
            lv_obj_clear_flag(Wallpaper_State().ui.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (Wallpaper_State().ui.page_lbl != nullptr) {
            lv_obj_set_width(Wallpaper_State().ui.page_lbl, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(Wallpaper_State().ui.page_lbl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(Wallpaper_State().ui.page_lbl, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_clear_flag(Wallpaper_State().ui.page_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(Wallpaper_State().ui.page_lbl);
        }
        if (Wallpaper_State().ui.status_label != nullptr && lv_obj_is_valid(Wallpaper_State().ui.status_label)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), Lang::Strings::WALLPAPER_SELECTED_FMT, WpSelectedCount());
            lv_label_set_text(Wallpaper_State().ui.status_label, buf);
        }
    } else {
        if (Wallpaper_State().ui.multi_bar != nullptr) {
            lv_obj_add_flag(Wallpaper_State().ui.multi_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(Wallpaper_State().ui.multi_bar, lv_pct(100));
            lv_obj_align(Wallpaper_State().ui.multi_bar, LV_ALIGN_CENTER, 0, 0);
        }
        if (Wallpaper_State().ui.page_lbl != nullptr) {
            lv_obj_set_width(Wallpaper_State().ui.page_lbl, LV_HOR_RES - 16);
            lv_obj_set_style_text_align(Wallpaper_State().ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(Wallpaper_State().ui.page_lbl, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(Wallpaper_State().ui.page_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (Wallpaper_State().ui.status_label != nullptr && lv_obj_is_valid(Wallpaper_State().ui.status_label)) {
            lv_label_set_text(Wallpaper_State().ui.status_label, Lang::Strings::HOME_APP_WALLPAPER);
        }
    }
}

void ExitWpMultiMode(bool rebuild) {
    Wallpaper_State().list.multi = false;
    Wallpaper_State().list.suppress_click_until_us = 0;
    Wallpaper_State().list.suppress_click_idx = -1;
    Wallpaper_State().list.selected.assign(Wallpaper_State().list.files.size(), 0);
    RefreshWpFooterMode();
    if (rebuild) {
        RequestWallpaperListRebuild();
    }
}

void EnterWpMultiModeSelect(int idx) {
    Wallpaper_State().list.multi = true;
    Wallpaper_State().list.selected.assign(Wallpaper_State().list.files.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(Wallpaper_State().list.selected.size())) {
        Wallpaper_State().list.selected[static_cast<size_t>(idx)] = 1;
    }
    RefreshWpFooterMode();
    RequestWallpaperListRebuild();
}

lv_obj_t* FindWpCell(int index) {
    if (Wallpaper_State().ui.list_host == nullptr || !lv_obj_is_valid(Wallpaper_State().ui.list_host)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(Wallpaper_State().ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(Wallpaper_State().ui.list_host, i);
        if (cell != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))) == index) {
            return cell;
        }
    }
    return nullptr;
}

lv_obj_t* FindWpCheckBox(lv_obj_t* cell) {
    if (cell == nullptr) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(cell);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(cell, i);
        if (c != nullptr && lv_obj_get_width(c) == kWpCheckSize &&
            lv_obj_get_height(c) == kWpCheckSize) {
            return c;
        }
    }
    return nullptr;
}

void PatchWpRowCheckMark(int index) {
    if (!Wallpaper_State().list.multi) {
        return;
    }
    lv_obj_t* check = FindWpCheckBox(FindWpCell(index));
    if (check == nullptr) {
        RequestWallpaperListRebuild();
        return;
    }
    lv_obj_clean(check);
    if (!WpItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

void PatchWpVisibleCheckMarks() {
    if (!Wallpaper_State().list.multi || Wallpaper_State().ui.list_host == nullptr || !lv_obj_is_valid(Wallpaper_State().ui.list_host)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(Wallpaper_State().ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(Wallpaper_State().ui.list_host, i);
        if (cell == nullptr) {
            continue;
        }
        PatchWpRowCheckMark(
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))));
    }
    if (Wallpaper_State().ui.status_label != nullptr && lv_obj_is_valid(Wallpaper_State().ui.status_label)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::WALLPAPER_SELECTED_FMT, WpSelectedCount());
        lv_label_set_text(Wallpaper_State().ui.status_label, buf);
    }
}

void RequestWpCheckMarksPaint() {
    if (Wallpaper_State().wp_check_paint.paint == nullptr) {
        Wallpaper_State().wp_check_paint.paint = PatchWpVisibleCheckMarks;
    }
    ScreenPaintCoalesceRequestDebounced(&Wallpaper_State().wp_check_paint, kWpCheckPaintDebounceUs);
}

void OnWpMultiCancel(lv_event_t* /*e*/) {
    if (Wallpaper_State().workers.delete_busy.load()) {
        return;
    }
    ExitWpMultiMode(true);
}

void OnWpMultiSelectAll(lv_event_t* /*e*/) {
    if (!Wallpaper_State().list.multi || Wallpaper_State().list.files.empty() || Wallpaper_State().workers.delete_busy.load()) {
        return;
    }
    SyncWpSelectedSize();
    const int n = static_cast<int>(Wallpaper_State().list.files.size());
    const bool clear = (WpSelectedCount() >= n);
    Wallpaper_State().list.selected.assign(static_cast<size_t>(n), clear ? 0 : 1);
    RequestWpCheckMarksPaint();
}

struct BatchDeleteItem {
    char path[192] = {};
    char name[96] = {};
};

struct BatchDeleteWork {
    uint32_t epoch = 0;
    int n = 0;
    BatchDeleteItem items[kMaxItems]{};
};

struct BatchDeleteResultMsg {
    uint32_t epoch = 0;
    int deleted = 0;
};

void ApplyBatchDeleteAsync(void* p) {
    auto* msg = static_cast<BatchDeleteResultMsg*>(p);
    Wallpaper_State().workers.delete_busy.store(false);
    Wallpaper_State().workers.delete_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        delete msg;
        return;
    }
    ESP_LOGI(TAG, "batch delete done n=%d", msg->deleted);
    RefreshActiveName();
    CollectWallpapers();
    Wallpaper_ClampListPage();
    ExitWpMultiMode(false);
    RebuildListPage();
    delete msg;
}

void BatchDeleteTask(void* arg) {
    auto* work = static_cast<BatchDeleteWork*>(arg);
    auto* msg = new BatchDeleteResultMsg{};
    if (work != nullptr) {
        msg->epoch = work->epoch;
        for (int i = 0; i < work->n; ++i) {
            std::string err;
            if (DeleteWallpaperFileOnSd(work->items[i].path, work->items[i].name, err)) {
                ++msg->deleted;
            } else {
                ESP_LOGW(TAG, "batch delete fail %s: %s", work->items[i].name, err.c_str());
            }
        }
        delete work;
    }
    if (lv_async_call(ApplyBatchDeleteAsync, msg) != LV_RESULT_OK) {
        delete msg;
        Wallpaper_State().workers.delete_busy.store(false);
        Wallpaper_State().workers.delete_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void OnWpMultiRemove(lv_event_t* /*e*/) {
    if (!Wallpaper_State().list.multi || Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.load_busy.load()) {
        return;
    }
    SyncWpSelectedSize();
    if (WpSelectedCount() <= 0) {
        return;
    }
    if (Wallpaper_State().workers.delete_busy.exchange(true)) {
        return;
    }
    auto* work = new BatchDeleteWork{};
    work->epoch = Wallpaper_State().epoch;
    for (int i = 0; i < static_cast<int>(Wallpaper_State().list.files.size()) && work->n < kMaxItems; ++i) {
        if (i >= static_cast<int>(Wallpaper_State().list.selected.size()) || Wallpaper_State().list.selected[static_cast<size_t>(i)] == 0) {
            continue;
        }
        auto& it = work->items[work->n++];
        std::snprintf(it.path, sizeof(it.path), "%s", Wallpaper_State().list.files[static_cast<size_t>(i)].path);
        std::snprintf(it.name, sizeof(it.name), "%s", Wallpaper_State().list.files[static_cast<size_t>(i)].name);
    }
    if (work->n <= 0) {
        delete work;
        Wallpaper_State().workers.delete_busy.store(false);
        return;
    }
    if (xTaskCreatePinnedToCore(BatchDeleteTask, "wp_batch_del", 8 * 1024, work, 5, &Wallpaper_State().workers.delete_task,
                                0) != pdPASS) {
        delete work;
        Wallpaper_State().workers.delete_busy.store(false);
        ESP_LOGW(TAG, "batch delete task create failed");
    }
}

void Wallpaper_OnRowLongPressed(lv_event_t* e) {
    if (Wallpaper_State().mode != UiMode::kList || Wallpaper_State().workers.delete_busy.load()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    if (!Wallpaper_State().list.multi) {
        Wallpaper_State().list.suppress_click_idx = index;
        Wallpaper_State().list.suppress_click_until_us = esp_timer_get_time() + kWpSuppressRowClickUs;
        EnterWpMultiModeSelect(index);
        return;
    }
    ToggleWpItemSelected(index);
    RequestWpCheckMarksPaint();
}

void Wallpaper_OnRowClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == Wallpaper_State().list.suppress_click_idx && esp_timer_get_time() < Wallpaper_State().list.suppress_click_until_us) {
        return;
    }
    if (Wallpaper_State().list.multi) {
        ToggleWpItemSelected(index);
        RequestWpCheckMarksPaint();
        return;
    }
    OpenPreview(index);
}

void BuildFooter(lv_obj_t* scr) {
    // 底栏槽：页码与多选共用，不改网格区高度
    lv_obj_t* foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, kFooterH);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    Wallpaper_DisableScroll(foot);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* page_lbl = lv_label_create(foot);
    lv_obj_set_width(page_lbl, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page_lbl, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);
    lv_label_set_text(page_lbl, "1/1");
    lv_obj_align(page_lbl, LV_ALIGN_CENTER, 0, 0);
    Wallpaper_DisableScroll(page_lbl);
    Wallpaper_State().ui.page_lbl = page_lbl;

    Wallpaper_State().ui.multi_bar = lv_obj_create(foot);
    lv_obj_remove_style_all(Wallpaper_State().ui.multi_bar);
    lv_obj_set_width(Wallpaper_State().ui.multi_bar, lv_pct(100));
    lv_obj_set_height(Wallpaper_State().ui.multi_bar, kWpMultiBtnH);
    lv_obj_set_style_bg_opa(Wallpaper_State().ui.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(Wallpaper_State().ui.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(Wallpaper_State().ui.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(Wallpaper_State().ui.multi_bar, 10, 0);
    lv_obj_align(Wallpaper_State().ui.multi_bar, LV_ALIGN_CENTER, 0, 0);
    Wallpaper_DisableScroll(Wallpaper_State().ui.multi_bar);
    lv_obj_clear_flag(Wallpaper_State().ui.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(Wallpaper_State().ui.multi_bar, LV_OBJ_FLAG_HIDDEN);

    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kWpMultiBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Wallpaper_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kWpUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
        lv_obj_set_style_pad_hor(text_wrap, kWpUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        Wallpaper_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kWpMultiDotSize, kWpMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        Wallpaper_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(Wallpaper_State().ui.multi_bar, Lang::Strings::COMMON_CANCEL, OnWpMultiCancel);
    make_multi_dot(Wallpaper_State().ui.multi_bar);
    make_multi_action(Wallpaper_State().ui.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnWpMultiSelectAll);
    make_multi_dot(Wallpaper_State().ui.multi_bar);
    make_multi_action(Wallpaper_State().ui.multi_bar, Lang::Strings::COMMON_REMOVE, OnWpMultiRemove);
}

