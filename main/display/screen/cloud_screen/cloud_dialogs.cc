#include "cloud_screen/cloud_dialogs.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/push/push_resources_library.h"

#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "screen_common.h"

void OnDlProgressCancelClicked(lv_event_t* /*e*/) {
    AbortDownloadJobFromUi();
}

void EnsureDlProgressPageBuilt() {
    auto& st = Cloud_State();
    if (st.xfer.dl_page != nullptr || st.chrome.screen == nullptr) {
        return;
    }

    lv_obj_t* page = lv_obj_create(st.chrome.screen);
    st.xfer.dl_page = page;
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(page, 0, 0);
    ScreenApplyDotBackdrop(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page);

    lv_obj_t* card = lv_obj_create(page);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - kDialogCardMargin, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kRowBorderW, 0);
    lv_obj_set_style_radius(card, kRowRadius, 0);
    lv_obj_set_style_pad_all(card, kDialogCardPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGap, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED,
                        nullptr);

    st.xfer.dl_title_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_title_lbl, Lang::Strings::CLOUD_SAVING);
    lv_obj_set_width(st.xfer.dl_title_lbl, LV_HOR_RES - kDialogLabelMargin);
    lv_obj_set_style_text_align(st.xfer.dl_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.xfer.dl_title_lbl, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(st.xfer.dl_title_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.xfer.dl_title_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.xfer.dl_count_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_count_lbl, Lang::Strings::CLOUD_SAVED_0_1);
    lv_obj_set_width(st.xfer.dl_count_lbl, LV_HOR_RES - kDialogLabelMargin);
    lv_obj_set_style_text_align(st.xfer.dl_count_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.xfer.dl_count_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.xfer.dl_count_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.xfer.dl_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_coord_t bar_w = kDlBarW;
    if (bar_w > LV_HOR_RES - kDialogLabelMargin) {
        bar_w = LV_HOR_RES - kDialogLabelMargin;
    }
    st.xfer.dl_bar = lv_bar_create(card);
    lv_obj_set_size(st.xfer.dl_bar, bar_w, kDlBarH);
    lv_bar_set_range(st.xfer.dl_bar, 0, 100);
    lv_bar_set_value(st.xfer.dl_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(st.xfer.dl_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st.xfer.dl_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(st.xfer.dl_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(st.xfer.dl_bar, kRowBorderW, LV_PART_MAIN);
    lv_obj_set_style_radius(st.xfer.dl_bar, kDlBarRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(st.xfer.dl_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(st.xfer.dl_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(st.xfer.dl_bar, kDlBarIndRadius, LV_PART_INDICATOR);
    lv_obj_clear_flag(st.xfer.dl_bar, LV_OBJ_FLAG_CLICKABLE);

    st.xfer.dl_pct_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_pct_lbl, "0%");
    lv_obj_set_style_text_font(st.xfer.dl_pct_lbl, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(st.xfer.dl_pct_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.xfer.dl_pct_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cancel_btn = lv_obj_create(card);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, kCancelBtnW, kCancelBtnH);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel_btn, kRowBorderW, 0);
    lv_obj_set_style_radius(cancel_btn, kRowRadius, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cancel_btn);
    lv_obj_add_event_cb(cancel_btn, OnDlProgressCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, Lang::Strings::COMMON_CANCEL);
    lv_obj_set_style_text_font(cancel_lbl, Cloud_UiFont(), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_clear_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void RefreshDlProgressPage(int file_percent) {
    auto& st = Cloud_State();
    if (!st.xfer.dl_page_open || st.xfer.dl_page == nullptr) {
        return;
    }
    if (file_percent < 0) {
        file_percent = 0;
    }
    if (file_percent > 100) {
        file_percent = 100;
    }
    if (st.xfer.dl_count_lbl != nullptr) {
        char buf[48];
        const int total = st.xfer.dl_job_total > 0 ? st.xfer.dl_job_total : 1;
        const int done = st.xfer.dl_job_done < 0 ? 0 : st.xfer.dl_job_done;
        std::snprintf(buf, sizeof(buf), Lang::Strings::CLOUD_SAVED_FMT, done, total);
        lv_label_set_text(st.xfer.dl_count_lbl, buf);
    }
    if (file_percent == st.xfer.dl_shown_percent) {
        return;
    }
    st.xfer.dl_shown_percent = file_percent;
    if (st.xfer.dl_bar != nullptr) {
        lv_bar_set_value(st.xfer.dl_bar, file_percent, LV_ANIM_OFF);
    }
    if (st.xfer.dl_pct_lbl != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", file_percent);
        lv_label_set_text(st.xfer.dl_pct_lbl, buf);
    }
}

void ShowDlProgressPage() {
    auto& st = Cloud_State();
    EnsureDlProgressPageBuilt();
    if (st.xfer.dl_page == nullptr) {
        return;
    }
    CloseDialog();
    CloseActionDialog();
    if (st.preview.open) {
        ++st.preview.epoch;
        st.preview.idx = -1;
        ClearWallpaperPreviewImage();
        ShowWallpaperPreviewMode(false);
    }
    st.xfer.dl_page_open = true;
    st.xfer.dl_shown_percent = -1;
    if (st.chrome.main_body != nullptr) {
        lv_obj_add_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.page_lbl != nullptr) {
        lv_obj_add_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.preview.body != nullptr) {
        lv_obj_add_flag(st.preview.body, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(st.xfer.dl_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(st.xfer.dl_page);
    // 遮罩盖住列表后下层热区仍按坐标早震；关掉热区早震，取消靠 PRESSED
    HapticSetZoneEarlyPulseEnabled(false);
    RefreshDlProgressPage(0);
}

void HideDlProgressPage() {
    auto& st = Cloud_State();
    st.xfer.dl_page_open = false;
    st.xfer.dl_shown_percent = -1;
    HapticSetZoneEarlyPulseEnabled(true);
    if (st.xfer.dl_page != nullptr) {
        lv_obj_add_flag(st.xfer.dl_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.main_body != nullptr && !st.preview.open) {
        lv_obj_clear_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.page_lbl != nullptr && !st.preview.open) {
        lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void CloseActionDialog() {
    auto& st = Cloud_State();
    if (st.dialogs.action_mask != nullptr && ScreenAlive()) {
        lv_obj_delete(st.dialogs.action_mask);
    }
    st.dialogs.action_mask = nullptr;
    st.xfer.action_index = -1;
}

void OnActionCancelClicked(lv_event_t* /*e*/) {
    CloseActionDialog();
}

void AsyncApplyDelete(void* p) {
    auto* msg = static_cast<CloudDeleteDoneMsg*>(p);
    auto& st = Cloud_State();
    st.data.delete_busy = false;
    if (!OpStillValid(msg->op_gen)) {
        delete msg;
        ResumeCloudSideHttpAfterDl();
        return;
    }
    // 批量摘行前先拆掉列表行（含缩略图控件），再释放 RasterImage。
    // 若只 Erase 后走 coalesce 延迟刷，中间 refr 会画悬空 lv_image src → LoadProhibited。
    if (st.chrome.list_body != nullptr) {
        lv_obj_clean(st.chrome.list_body);
    }
    for (const auto& id : msg->ok_ids) {
        const int idx = FindItemIndexByTaskId(id.c_str());
        if (idx >= 0) {
            EraseItemAt(idx);
        }
    }
    char buf[64];
    if (msg->ok_n > 0 && msg->fail_n == 0) {
        std::snprintf(buf, sizeof(buf), Lang::Strings::TASK_REMOVED_N_FMT, msg->ok_n);
        SetStatusTip(buf, true);
    } else if (msg->ok_n > 0) {
        std::snprintf(buf, sizeof(buf), Lang::Strings::TASK_BATCH_RESULT_FMT, msg->ok_n,
                      msg->fail_n);
        SetStatusTip(buf, true);
    } else {
        SetStatusTip(msg->error[0] != '\0' ? msg->error : Lang::Strings::CLOUD_DELETE_FAIL, true);
    }
    ExitMultiModeEx(false, false);
    RenderListPage();  // 同步重建，勿 RequestCloudRender 合并延迟
    ResumeCloudSideHttpAfterDl();
    delete msg;
}

void CloudDeleteWorker(void* arg) {
    auto* work = static_cast<CloudDeleteWork*>(arg);
    auto* msg = new CloudDeleteDoneMsg{};
    msg->op_gen = work->op_gen;
    for (const auto& item : work->items) {
        std::string err;
        bool ok = reader::DeletePushResourceRemote(item, err);
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(400));
            err.clear();
            ok = reader::DeletePushResourceRemote(item, err);
        }
        if (ok) {
            msg->ok_ids.push_back(item.task_id);
            ++msg->ok_n;
        } else {
            ++msg->fail_n;
            if (msg->error[0] == '\0') {
                std::snprintf(msg->error, sizeof(msg->error), "%s",
                              err.empty() ? Lang::Strings::CLOUD_DELETE_FAIL : err.c_str());
            }
        }
    }
    delete work;
    s_delete_task = nullptr;
    if (!ScreenLvAsync(AsyncApplyDelete, msg)) {
        ESP_LOGW(TAG, "ScreenLvAsync delete failed");
        const uint32_t gen = msg->op_gen;
        delete msg;
        if (ScreenAlive() && Cloud_State().xfer.op_generation == gen) {
            Cloud_State().data.delete_busy = false;
        }
    }
    vTaskDelete(nullptr);
}

void ScheduleDeleteItems(std::vector<reader::CloudPushResource> items) {
    auto& st = Cloud_State();
    if (items.empty()) {
        SetStatusTip(Lang::Strings::CLOUD_SELECT_FIRST, true);
        return;
    }
    if (st.data.delete_busy || CloudDlTasksActive() || SyncBusy() || s_delete_task != nullptr) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    auto* work = new CloudDeleteWork{};
    work->op_gen = st.xfer.op_generation;
    work->items = std::move(items);
    st.data.delete_busy = true;
    st.data.multi = false;
    RefreshFooterMode();
    SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, false);
    // 与下载相同：打断封面 HTTP，避免删行时仍回写悬空缩略图
    AbortCloudSideHttpForDownload();
    if (xTaskCreatePinnedToCore(CloudDeleteWorker, "cloud_res_del", kCloudDeleteStack, work,
                                tskIDLE_PRIORITY + 2, &s_delete_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "delete task create failed");
        delete work;
        s_delete_task = nullptr;
        st.data.delete_busy = false;
        SetStatusTip(Lang::Strings::CLOUD_DELETE_FAIL, true);
        ResumeCloudSideHttpAfterDl();
        return;
    }
}

void Cloud_OnDeleteClicked(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    const int index = st.xfer.action_index;
    CloseActionDialog();
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    std::vector<reader::CloudPushResource> items;
    items.push_back(st.data.items[static_cast<size_t>(index)]);
    ScheduleDeleteItems(std::move(items));
}

void ShowActionDialog(int cloud_index) {
    auto& st = Cloud_State();
    if (!ScreenAlive() || st.dialogs.action_mask != nullptr || st.dialogs.dialog_mask != nullptr ||
        st.xfer.download_busy || st.data.delete_busy || SyncBusy()) {
        return;
    }
    if (cloud_index < 0 || cloud_index >= static_cast<int>(st.data.items.size())) {
        return;
    }

    const reader::CloudPushResource& item = st.data.items[static_cast<size_t>(cloud_index)];
    const lv_coord_t card_w = LV_HOR_RES - kDialogCardMargin;
    const lv_coord_t text_w = card_w - kActionDialogPadInset;
    // 单行省略书名/文件名，避免 WRAP 顶掉底部按钮
    const std::string short_name = EllipsizeText(item.name, Cloud_UiFont(), text_w);
    char title_buf[192];
    std::snprintf(title_buf, sizeof(title_buf), "「%s」\n%s", short_name.c_str(), item.TypeLabel());
    st.xfer.action_index = cloud_index;

    lv_obj_t* mask = lv_obj_create(st.chrome.screen);
    st.dialogs.action_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(mask);
    lv_obj_add_event_cb(mask, OnActionCancelClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, kActionDialogCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kRowBorderW, 0);
    lv_obj_set_style_radius(card, kRowRadius, 0);
    lv_obj_set_style_pad_all(card, kDialogCardPadSmall, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGapSmall, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, title_buf);
    lv_obj_set_width(lbl, text_w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_max_height(lbl, lv_font_get_line_height(Cloud_UiFont()) * 3, 0);
    lv_obj_set_style_text_font(lbl, Cloud_UiFont(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, kActionBtnRowH);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);

    auto make_btn = [&](const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kActionBtnW, kActionBtnH);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, kRowBorderW, 0);
        lv_obj_set_style_radius(btn, kRowRadius, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* bl = lv_label_create(btn);
        lv_label_set_text(bl, text);
        lv_obj_set_style_text_font(bl, Cloud_UiFont(), 0);
        lv_obj_center(bl);
        lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);
    };
    make_btn(Lang::Strings::COMMON_DELETE, Cloud_OnDeleteClicked);
    make_btn(Lang::Strings::COMMON_CANCEL, OnActionCancelClicked);
}

void CloseDialog() {
    auto& st = Cloud_State();
    if (st.dialogs.dialog_mask != nullptr && ScreenAlive()) {
        lv_obj_delete(st.dialogs.dialog_mask);
    }
    st.dialogs.dialog_mask = nullptr;
    st.xfer.pending_index = -1;
}

