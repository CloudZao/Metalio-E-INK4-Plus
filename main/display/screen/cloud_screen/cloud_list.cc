#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_dialogs.h"
#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/push/push_resources_library.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "reader/reader.h"

void Cloud_OnRowLongPressed(lv_event_t* e) {
    auto& st = Cloud_State();
    if (st.xfer.dl_page_open || st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || SyncBusy() ||
        st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr || st.preview.open) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    if (!st.data.multi) {
        st.data.suppress_row_click_idx = index;
        st.data.suppress_row_click_until_us = esp_timer_get_time() + kSuppressRowClickUs;
        EnterMultiModeSelect(index);
        return;
    }
    ToggleItemSelected(index);
    RequestCheckMarksPaint();
}

void Cloud_OnRowClicked(lv_event_t* e) {
    auto& st = Cloud_State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == st.data.suppress_row_click_idx &&
        esp_timer_get_time() < st.data.suppress_row_click_until_us) {
        return;
    }
    if (st.xfer.dl_page_open || SyncBusy() || st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    if (st.data.multi) {
        ToggleItemSelected(index);
        RequestCheckMarksPaint();
        return;
    }
    OpenWallpaperPreview(index);
}

void OnMultiCancel(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    if (st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy) {
        return;
    }
    ExitMultiMode(true);
}

void OnMultiSelectAll(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    if (!st.data.multi || st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy) {
        return;
    }
    RebuildFiltered();
    SyncSelectedSize();
    if (st.data.filtered.empty()) {
        return;
    }
    int selected_n = 0;
    for (int idx : st.data.filtered) {
        if (ItemSelected(idx)) {
            ++selected_n;
        }
    }
    const bool clear = (selected_n >= static_cast<int>(st.data.filtered.size()));
    for (int idx : st.data.filtered) {
        if (idx >= 0 && idx < static_cast<int>(st.data.selected.size())) {
            st.data.selected[static_cast<size_t>(idx)] = clear ? 0 : 1;
        }
    }
    RequestCheckMarksPaint();
}

void OnMultiDelete(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    if (!st.data.multi || st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy || SyncBusy()) {
        return;
    }
    SyncSelectedSize();
    std::vector<reader::CloudPushResource> items;
    for (size_t i = 0; i < st.data.items.size() && i < st.data.selected.size(); ++i) {
        if (st.data.selected[i] != 0) {
            items.push_back(st.data.items[i]);
        }
    }
    ScheduleDeleteItems(std::move(items));
}

void OnMultiSave(lv_event_t* /*e*/) {
    auto& st = Cloud_State();
    if (!st.data.multi || st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy || SyncBusy()) {
        return;
    }
    SyncSelectedSize();
    st.data.batch_save_ids.clear();
    for (size_t i = 0; i < st.data.items.size() && i < st.data.selected.size(); ++i) {
        if (st.data.selected[i] != 0) {
            st.data.batch_save_ids.push_back(st.data.items[i].task_id);
        }
    }
    if (st.data.batch_save_ids.empty()) {
        SetStatusTip(Lang::Strings::CLOUD_SELECT_FIRST, true);
        return;
    }
    st.data.batch_busy = true;
    st.xfer.dl_job_total = static_cast<int>(st.data.batch_save_ids.size());
    st.xfer.dl_job_done = 0;
    st.xfer.dl_user_abort = false;
    SetStatusTip("", false);
    st.data.multi = false;
    RefreshFooterMode();
    ShowDlProgressPage();
    ContinueBatchSaveIfNeeded();
}

lv_obj_t* FindListRow(int index) {
    auto& st = Cloud_State();
    if (st.chrome.list_body == nullptr || !lv_obj_is_valid(st.chrome.list_body)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(st.chrome.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(st.chrome.list_body, i);
        if (row != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))) == index) {
            return row;
        }
    }
    return nullptr;
}

lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h) {
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(row, i);
        if (c != nullptr && lv_obj_get_width(c) == w && lv_obj_get_height(c) == h) {
            return c;
        }
    }
    return nullptr;
}

void PatchRowCheckMark(int index) {
    auto& st = Cloud_State();
    if (!st.data.multi) {
        return;
    }
    lv_obj_t* row = FindListRow(index);
    lv_obj_t* check = FindRowChildBySize(row, kCheckSize, kCheckSize);
    if (check == nullptr) {
        RequestCloudRender(); // 勾选列尚未建好（进多选 coalesce 未落地）
        return;
    }
    lv_obj_clean(check);
    if (!ItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

void PatchVisibleRowCheckMarks() {
    auto& st = Cloud_State();
    if (!st.data.multi || st.chrome.list_body == nullptr || !lv_obj_is_valid(st.chrome.list_body)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(st.chrome.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(st.chrome.list_body, i);
        if (row == nullptr) {
            continue;
        }
        PatchRowCheckMark(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::CLOUD_SELECTED_FMT, SelectedCount());
    SetStatusTip(buf, false);
}

void PatchRowThumb(int index) {
    auto& st = Cloud_State();
    if (index < 0 || index >= static_cast<int>(st.data.thumbs.size()) ||
        st.data.thumbs[static_cast<size_t>(index)] == nullptr ||
        st.data.thumbs[static_cast<size_t>(index)]->empty()) {
        return;
    }
    lv_obj_t* row = FindListRow(index);
    lv_obj_t* thumb_box = FindRowChildBySize(row, kThumbW, kThumbH);
    if (thumb_box == nullptr) {
        return;  // 非本页或无封面行：数据已写入 st.data.thumbs，翻页再建
    }
    lv_obj_clean(thumb_box);
    auto& img = *st.data.thumbs[static_cast<size_t>(index)];
    lv_obj_t* thumb = lv_image_create(thumb_box);
    lv_image_set_src(thumb, &img.dsc);
    lv_obj_set_size(thumb, img.width, img.height);
    lv_obj_center(thumb);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateResourceRow(lv_obj_t* parent, const reader::CloudPushResource& item, int index) {
    auto& st = Cloud_State();
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // 以外框内容宽为准：需扣 list_frame 的 pad + border，否则右边框竖线被裁切
    lv_coord_t row_w = parent != nullptr ? lv_obj_get_content_width(parent) : 0;
    if (row_w <= 0) {
        row_w = Cloud_ContentWidth() - kListFramePad * 2 - kListFrameBorderW * 2;
    }
    lv_obj_set_size(row, row_w, kCloudRowH);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, kRowBorderW, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_radius(row, kRowRadius, 0);
    lv_obj_set_style_pad_hor(row, kRowPad, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(row);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, Cloud_OnRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(row, Cloud_OnRowLongPressed, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    const bool show_thumb = item.type == reader::PushResourceType::kBook ||
                            item.type == reader::PushResourceType::kBadge ||
                            item.type == reader::PushResourceType::kFont;
    lv_coord_t text_x = 0;
    if (show_thumb) {
        lv_obj_t* thumb_box = lv_obj_create(row);
        lv_obj_remove_style_all(thumb_box);
        lv_obj_set_size(thumb_box, kThumbW, kThumbH);
        lv_obj_set_style_border_width(thumb_box, 2, 0);
        lv_obj_set_style_border_color(thumb_box, lv_color_black(), 0);
        lv_obj_set_style_bg_color(thumb_box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(thumb_box, LV_OPA_COVER, 0);
        lv_obj_align(thumb_box, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(thumb_box, LV_OBJ_FLAG_CLICKABLE);
        Cloud_DisableScroll(thumb_box);

        if (index >= 0 && index < static_cast<int>(st.data.thumbs.size()) &&
            st.data.thumbs[static_cast<size_t>(index)] != nullptr &&
            !st.data.thumbs[static_cast<size_t>(index)]->empty()) {
            lv_obj_t* thumb = lv_image_create(thumb_box);
            lv_image_set_src(thumb, &st.data.thumbs[static_cast<size_t>(index)]->dsc);
            lv_obj_set_size(thumb, st.data.thumbs[static_cast<size_t>(index)]->width,
                            st.data.thumbs[static_cast<size_t>(index)]->height);
            lv_obj_center(thumb);
            lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
        }
        text_x = kThumbW + kThumbGap;
    }

    const lv_font_t* item_font = Cloud_ItemFont();
    const lv_coord_t line_h =
        (item_font != nullptr && item_font->line_height > 0) ? item_font->line_height : 30;
    const bool show_check = st.data.multi;
    const lv_coord_t trail_reserve = show_check ? (kCheckSize + kCheckGap) : 0;
    const lv_coord_t text_w = row_w - kRowPad * 2 - text_x - trail_reserve;
    const std::string line1 = EllipsizeText(item.name, item_font, text_w);

    lv_obj_t* title = lv_label_create(row);
    lv_obj_set_style_text_font(title, item_font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_size(title, text_w, line_h);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(title, line1.c_str());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, show_thumb ? 4 : 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    char line2[80];
    std::snprintf(line2, sizeof(line2), "%s · %s", item.TypeLabel(),
                  reader::FormatCloudFileSize(item.file_size).c_str());
    lv_obj_t* meta = lv_label_create(row);
    lv_obj_set_style_text_font(meta, item_font, 0);
    lv_obj_set_style_text_color(meta, lv_color_black(), 0);
    lv_obj_set_size(meta, text_w, line_h);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_CLIP);
    lv_label_set_text(meta, line2);
    lv_obj_align(meta, LV_ALIGN_TOP_LEFT, text_x,
                 (show_thumb ? 4 : 0) + line_h + kCloudRowLineGap);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);

    if (show_check) {
        // 行尾勾选：白底黑框；选中只画 √，勿实心填充（同每日清单）
        lv_obj_t* check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kCheckSize, kCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, kRowBorderW, 0);
        lv_obj_set_style_radius(check, 4, 0);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
        Cloud_DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        if (ItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, item_font, 0);
            lv_obj_set_style_text_color(mark, lv_color_black(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    return row;
}

void RenderListPageInternal(bool schedule_thumbs) {
    auto& st = Cloud_State();
    if (st.chrome.list_body == nullptr) {
        return;
    }
    lv_obj_set_style_layout(st.chrome.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.chrome.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.chrome.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.chrome.list_body, kRowGap, 0);
    lv_obj_clean(st.chrome.list_body);
    Cloud_DisableScroll(st.chrome.list_body);

    // 拉取中若已有列表：保留行，提示在标题旁；仅空列表时占位
    if (st.data.waiting_net && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, Lang::Strings::CLOUD_CONNECT_NET);
        SetPageFooterText("");
        return;
    }
    if (st.data.loading && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, Lang::Strings::CLOUD_FETCHING_LIST);
        SetPageFooterText("");
        return;
    }
    if (st.data.covers_loading && !st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, Lang::Strings::CLOUD_LOAD_COVER);
        SetPageFooterText("");
        return;
    }
    if (!st.data.error.empty() && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, st.data.error.c_str());
        SetPageFooterText("1 / 1");
        return;
    }

    RebuildFiltered();
    if (st.data.filtered.empty()) {
        const char* empty_msg = Lang::Strings::CLOUD_EMPTY_ALL;
        if (st.data.filter_tab == 1) {
            empty_msg = Lang::Strings::CLOUD_EMPTY_WALLPAPER;
        } else if (st.data.filter_tab == 2) {
            empty_msg = Lang::Strings::CLOUD_EMPTY_BOOK;
        } else if (st.data.filter_tab == 3) {
            empty_msg = Lang::Strings::CLOUD_EMPTY_FONT;
        }
        Cloud_ShowMessage(st.chrome.list_body, empty_msg);
        SetPageFooterText("1 / 1");
        return;
    }

    ClampPage();
    const int start = st.data.page * st.data.page_size;
    const int end = std::min(start + st.data.page_size, static_cast<int>(st.data.filtered.size()));
    for (int fi = start; fi < end; ++fi) {
        const int i = st.data.filtered[static_cast<size_t>(fi)];
        CreateResourceRow(st.chrome.list_body, st.data.items[static_cast<size_t>(i)], i);
    }

    {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d / %d", st.data.page + 1, PageCount());
        SetPageFooterText(foot);
    }

    if (schedule_thumbs && !st.data.loading && !st.data.waiting_net && !st.data.covers_loading &&
        !st.xfer.download_busy && !st.data.batch_busy && !s_dl_owns_http.load(std::memory_order_acquire)) {
        ScheduleCoverThumbFill();
    }
    RefreshFooterMode();
}

void RenderListPage() {
    RenderListPageInternal(true);
}

void RequestCloudRender() {
    if (s_cloud_paint.paint == nullptr) {
        s_cloud_paint.paint = RenderListPage;
    }
    ScreenPaintCoalesceRequest(&s_cloud_paint);
}

void RequestCheckMarksPaint() {
    if (s_cloud_check_paint.paint == nullptr) {
        s_cloud_check_paint.paint = PatchVisibleRowCheckMarks;
    }
    // 点间隙手指会抬起：勿立刻上屏，停手后再一次刷全部 √
    ScreenPaintCoalesceRequestDebounced(&s_cloud_check_paint, 280000);
}

