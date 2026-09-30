#include "cloud_screen/cloud_chrome.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_fetch.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"

#include <lvgl.h>
#include <cstdint>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "board.h"
#include "haptic_feedback.h"

void ScheduleUserCloudSync() {
    auto& st = Cloud_State();
    if (st.preview.open) {
        return;
    }
    if (st.xfer.download_busy || SyncBusy() || s_net_prep_task != nullptr) {
        return;
    }
    if (Board::GetInstance().IsWifiConfigMode()) {
        st.data.error = Lang::Strings::CLOUD_NEED_WIFI_CFG;
        RenderListPage();
        return;
    }
    st.data.fetch_done = false;
    ScheduleNetPrepAndFetch();
}

void OnTabClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = Cloud_State();
    // Tab 仅本地筛选；下载进度页打开时不可切
    if (st.xfer.dl_page_open || st.preview.open || st.dialogs.dialog_mask != nullptr ||
        st.dialogs.action_mask != nullptr) {
        return;
    }
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kTabCount || tab == st.data.filter_tab) {
        return;
    }
    // 多选跨 Tab 保留选中；仅换筛选与列表
    st.data.filter_tab = tab;
    st.data.page = 0;
    RefreshTabUi();
    RenderListPage();
}

lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab) {
    auto& st = Cloud_State();
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_pad_all(btn, 0, 0);
    Cloud_DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnTabClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(tab)));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Cloud_ItemFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.tab_btns[tab] = btn;
    st.chrome.tab_lbls[tab] = lbl;
    return btn;
}

void OnSyncClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = Cloud_State();
    if (st.preview.open || st.data.multi) {
        return;
    }
    if (st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || SyncBusy() ||
        s_net_prep_task != nullptr) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_user_sync_us != 0 && (now - s_last_user_sync_us) < kSyncThrottleUs) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    s_last_user_sync_us = now;
    ScheduleUserCloudSync();
}

void BuildCloudMainChrome(lv_obj_t* scr, lv_coord_t status_height) {
    auto& st = Cloud_State();
    // 为贴底页码留出 kFooterOutsideH，列表区可多挤一行
    const lv_coord_t body_h = LV_VER_RES - status_height - kFooterOutsideH;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    Cloud_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.main_body = body;

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, Cloud_ContentWidth());
    lv_obj_set_height(tab_row, kTabH);
    lv_obj_set_style_bg_color(tab_row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tab_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_row, lv_color_black(), 0);
    lv_obj_set_style_border_width(tab_row, kTabBorderW, 0);
    lv_obj_set_style_radius(tab_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(tab_row, true, 0);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_row, kTabInset, 0);
    lv_obj_set_style_pad_column(tab_row, 0, 0);
    Cloud_DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_ALL, 0);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_WALLPAPER, 1);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_BOOK, 2);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_FONT, 3);
    RefreshTabUi();

    lv_obj_t* header_row = lv_obj_create(body);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, Cloud_ContentWidth());
    lv_obj_set_height(header_row, kHeaderH);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, kHeaderGap, 0);
    Cloud_DisableScroll(header_row);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star_wrap = lv_obj_create(header_row);
    lv_obj_remove_style_all(star_wrap);
    lv_obj_set_size(star_wrap, kHeaderIcon, kHeaderIcon);
    lv_obj_set_style_bg_color(star_wrap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(star_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(star_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(star_wrap, 0, 0);
    Cloud_DisableScroll(star_wrap);
    lv_obj_clear_flag(star_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star = lv_label_create(star_wrap);
    lv_label_set_text(star, "\xE2\x98\x85");  // ★
    lv_obj_set_style_text_color(star, lv_color_white(), 0);
    lv_obj_set_style_text_font(star, Cloud_ItemFont(), 0);
    lv_obj_center(star);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.section_title = lv_label_create(header_row);
    lv_label_set_text(st.chrome.section_title, Lang::Strings::CLOUD_PENDING);
    lv_obj_set_style_text_font(st.chrome.section_title, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.section_title, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.section_title, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.status_lbl = lv_label_create(header_row);
    lv_label_set_text(st.chrome.status_lbl, "");
    lv_obj_set_style_pad_left(st.chrome.status_lbl, kStatusGap, 0);
    lv_obj_set_style_text_font(st.chrome.status_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);

    // 细外框：包住列表 + 刷新；页码贴底在框外
    lv_obj_t* list_frame = lv_obj_create(body);
    lv_obj_remove_style_all(list_frame);
    lv_obj_set_width(list_frame, Cloud_ContentWidth());
    lv_obj_set_flex_grow(list_frame, 1);
    lv_obj_set_style_bg_color(list_frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(list_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_frame, kListFrameBorderW, 0);
    lv_obj_set_style_border_color(list_frame, lv_color_black(), 0);
    lv_obj_set_style_radius(list_frame, kListFrameRadius, 0);
    lv_obj_set_style_pad_all(list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_frame, 0, 0);
    Cloud_DisableScroll(list_frame);
    lv_obj_clear_flag(list_frame, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(list_frame);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, lv_pct(100));
    lv_obj_set_flex_grow(list_body, 1);
    Cloud_DisableScroll(list_body);
    st.chrome.list_body = list_body;

    lv_obj_t* footer = lv_obj_create(list_frame);
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, kFooterBlockH);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, kSyncDividerW, 0);
    lv_obj_set_style_border_color(footer, lv_color_black(), 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(footer);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.footer = footer;

    // 分割线 + 居中「刷新」下划线（多选时隐藏，换 multi_bar）
    st.chrome.sync_btn = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.sync_btn);
    lv_obj_set_width(st.chrome.sync_btn, lv_pct(100));
    lv_obj_set_height(st.chrome.sync_btn, kSyncBtnH);
    lv_obj_set_style_bg_opa(st.chrome.sync_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.sync_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.chrome.sync_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(st.chrome.sync_btn);
    lv_obj_add_flag(st.chrome.sync_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(st.chrome.sync_btn);
    lv_obj_add_event_cb(st.chrome.sync_btn, OnSyncClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sync_text = lv_obj_create(st.chrome.sync_btn);
    lv_obj_remove_style_all(sync_text);
    lv_obj_set_size(sync_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sync_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sync_text, kSyncUnderlineH, 0);
    lv_obj_set_style_border_color(sync_text, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(sync_text, 2, 0);
    lv_obj_set_style_pad_hor(sync_text, kSyncUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(sync_text, LV_OPA_TRANSP, 0);
    Cloud_DisableScroll(sync_text);
    lv_obj_clear_flag(sync_text, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.sync_lbl = lv_label_create(sync_text);
    lv_label_set_text(st.chrome.sync_lbl, Lang::Strings::CLOUD_REFRESH);
    lv_obj_set_style_text_font(st.chrome.sync_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.sync_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.sync_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.multi_bar = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.multi_bar);
    lv_obj_set_width(st.chrome.multi_bar, lv_pct(100));
    lv_obj_set_height(st.chrome.multi_bar, kSyncBtnH);
    lv_obj_set_style_bg_opa(st.chrome.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.chrome.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.chrome.multi_bar, kMultiBarGap, 0);
    Cloud_DisableScroll(st.chrome.multi_bar);
    lv_obj_clear_flag(st.chrome.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);

    // 与「刷新」同款：下划线文字；多项间居中「·」
    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kSyncBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, kMultiActionPadHor, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Cloud_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kSyncUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, kSyncUnderlinePadBottom, 0);
        lv_obj_set_style_pad_hor(text_wrap, kSyncUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        Cloud_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Cloud_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kMultiDotSize, kMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        Cloud_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_CANCEL, OnMultiCancel);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnMultiSelectAll);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_DELETE, OnMultiDelete);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::CLOUD_SAVE, OnMultiSave);

    // 页码贴底，对齐壁纸页
    st.chrome.page_lbl = lv_label_create(scr);
    lv_label_set_text(st.chrome.page_lbl, "");
    lv_obj_set_width(st.chrome.page_lbl, LV_HOR_RES - kPageLblMargin);
    lv_obj_set_style_text_align(st.chrome.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.chrome.page_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.page_lbl, lv_color_black(), 0);
    lv_obj_align(st.chrome.page_lbl, LV_ALIGN_BOTTOM_MID, 0, -kPageLblBottom);
    lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(st.chrome.page_lbl);

    // 固定 6 条/页（Tab+标题后列表区刚好可排下）
    st.data.page_size = 6;
}

void BuildWallpaperPreviewChrome(lv_obj_t* scr, lv_coord_t status_height) {
    auto& st = Cloud_State();
    // 预览：图框 → 双行文件名 → 类型/大小胶囊 → 保存（对齐壁纸设置页）
    lv_obj_t* preview_body = lv_obj_create(scr);
    lv_obj_remove_style_all(preview_body);
    lv_obj_set_size(preview_body, LV_HOR_RES, LV_VER_RES - status_height);
    lv_obj_align(preview_body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(preview_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(preview_body, kPad, 0);
    lv_obj_set_style_pad_top(preview_body, kPreviewBodyPadTop, 0);
    lv_obj_set_style_pad_bottom(preview_body, kPreviewBodyPadBot, 0);
    lv_obj_set_flex_flow(preview_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(preview_body, kPreviewGap, 0);
    Cloud_DisableScroll(preview_body);
    lv_obj_add_flag(preview_body, LV_OBJ_FLAG_HIDDEN);
    st.preview.body = preview_body;

    lv_obj_t* img_host = lv_obj_create(preview_body);
    lv_obj_remove_style_all(img_host);
    lv_obj_set_width(img_host, Cloud_ContentWidth());
    lv_obj_set_flex_grow(img_host, 1);
    lv_obj_set_style_max_height(img_host, kPreviewImgMaxH, 0);
    lv_obj_set_style_bg_color(img_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(img_host, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(img_host, kRowBorderW, 0);
    lv_obj_set_style_border_color(img_host, lv_color_black(), 0);
    lv_obj_set_style_radius(img_host, kPreviewImgHostRadius, 0);
    lv_obj_set_style_clip_corner(img_host, true, 0);
    Cloud_DisableScroll(img_host);
    st.preview.img_host = img_host;

    lv_obj_t* img = lv_image_create(img_host);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    st.preview.img = img;

    lv_obj_t* preview_status = lv_label_create(img_host);
    lv_obj_set_style_text_font(preview_status, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(preview_status, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(preview_status, "");
    lv_obj_align(preview_status, LV_ALIGN_CENTER, 0, 0);
    st.preview.status = preview_status;

    lv_obj_t* preview_title = lv_label_create(preview_body);
    lv_obj_set_width(preview_title, Cloud_ContentWidth());
    {
        const lv_font_t* f = Cloud_UiFont();
        const lv_coord_t lh = (f != nullptr && f->line_height > 0) ? f->line_height : 29;
        lv_obj_set_height(preview_title, lh * 2);
    }
    lv_obj_set_style_text_font(preview_title, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(preview_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(preview_title, 0, 0);
    // 文案已按像素拆成最多两行；CLIP 避免再走 LVGL 整词换行
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(preview_title, "");
    st.preview.title = preview_title;

    lv_obj_t* preview_meta = lv_obj_create(preview_body);
    lv_obj_remove_style_all(preview_meta);
    lv_obj_set_width(preview_meta, Cloud_ContentWidth());
    lv_obj_set_height(preview_meta, kPreviewMetaH);
    lv_obj_set_flex_flow(preview_meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview_meta, kPreviewMetaGap, 0);
    lv_obj_clear_flag(preview_meta, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(preview_meta);
    st.preview.meta = preview_meta;

    lv_obj_t* download_btn = lv_obj_create(preview_body);
    lv_obj_remove_style_all(download_btn);
    lv_obj_set_size(download_btn, Cloud_ContentWidth(), kActionH);
    lv_obj_set_style_pad_all(download_btn, 0, 0);
    lv_obj_set_flex_flow(download_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(download_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(download_btn);
    HapticAttachClick(download_btn);
    lv_obj_add_event_cb(download_btn, OnWallpaperDownloadClicked, LV_EVENT_CLICKED, nullptr);
    st.preview.download_btn = download_btn;

    lv_obj_t* download_lbl = lv_label_create(download_btn);
    lv_obj_set_style_text_font(download_lbl, Cloud_UiFont(), 0);
    lv_label_set_text(download_lbl, Lang::Strings::CLOUD_SAVE_LOCAL);
    st.preview.download_lbl = download_lbl;
    StyleDownloadBtn(true);
}

