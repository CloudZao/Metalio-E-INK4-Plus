#pragma GCC optimize("O1")

#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include "epd_refresh.h"
#include <cstdio>

void SetReadStatusVisible(bool visible) {
    auto& st = Book_State();
    if (st.reader.status_bar != nullptr && lv_obj_is_valid(st.reader.status_bar)) {
        if (visible) {
            // 盖在正文上时需白底，否则字迹透出
            lv_obj_set_style_bg_color(st.reader.status_bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(st.reader.status_bar, LV_OPA_COVER, 0);
            lv_obj_clear_flag(st.reader.status_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.reader.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(st.reader.status_bar, LV_OPA_TRANSP, 0);
        }
    }
    if (st.reader.status_overlay != nullptr && lv_obj_is_valid(st.reader.status_overlay)) {
        // overlay 叠在图标行上，保持透明以免盖住电量/网络
        if (visible) {
            lv_obj_set_style_bg_opa(st.reader.status_overlay, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(st.reader.status_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.reader.status_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 状态栏中央（时钟位）：有文案则显示，nullptr/空则藏起
void SetReadStatusCenterTitle(const char* title) {
    auto& st = Book_State();
    if (st.reader.status_label == nullptr || !lv_obj_is_valid(st.reader.status_label)) {
        return;
    }
    if (title != nullptr && title[0] != '\0') {
        lv_label_set_text(st.reader.status_label, title);
        lv_obj_clear_flag(st.reader.status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(st.reader.status_label, "");
        lv_obj_add_flag(st.reader.status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void SetReadSettingsSheetVisible(bool visible) {
    if (ReaderAutoTurnEnabled()) ResetReaderAutoTurnDeadline();
    auto& st = Book_State();
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    if (visible) {
        ApplyReaderChromeSize();
        lv_obj_clear_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN);
    } else {
        const bool was_visible = lv_obj_is_visible(st.settings.settings_sheet);
        lv_obj_add_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN);
        if (was_visible && !ReaderPageImagesHint()) EpdRequestBlackEqualization();
    }
}

void HideReadChrome() {
    CancelReadFontScan();
    CloseBookmarkMessage();
    ResetBookmarkSelection();
    auto& st = Book_State();
    st.settings.reader_sheet_scrolling = false;
    EpdSetUiScrolling(false);
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
    TapZonesPanelClose();
    TtfPanelClose();
    st.settings.font_multi = false;
    st.settings.font_suppress_click_until_us = 0;
    st.settings.font_suppress_click_idx = -1;
    st.settings.font_suppress_next_click = false;
    st.settings.font_selected.assign(st.settings.font_entries.size(), 0);
    SetReadStatusCenterTitle(nullptr);
    SetReadStatusVisible(false);
    SetReadSettingsSheetVisible(false);
    const bool was_overlay = IsReadOverlayChrome(st.reader.read_chrome);
    const bool was_settings = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    st.reader.settings_resume_after_open = false;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    // 设置卡片盖在正文上：关掉即可，不必重绘；目录整页需恢复正文
    SyncReaderAaForImmersion(true);
    if (was_overlay && !was_settings && st.reader.session && st.reader.session->IsOpen() &&
        st.reader.content != nullptr) {
        CancelListCoverFill();  // 目录封面槽将随 content 重建销毁
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        RenderReaderPage();
    } else if (was_settings) {
        // 仅提交尚未应用的排版；未改设置时保留当前正文。
        FlushLayoutDebounce();
        // 目录与字体列表可持有大量控件，关闭后释放卡片内容。
        CancelListCoverFill();
        ClearSettingsSheetWidgetRefs();
        if (st.settings.settings_sheet && lv_obj_is_valid(st.settings.settings_sheet))
            lv_obj_clean(st.settings.settings_sheet);
        st.settings.reader_sheet_cover.Reset();
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        // 打开漫画设置时已取消图片解码，关闭后恢复当前页及预取。
        if (ReaderPageImagesHint()) RenderReaderPage();
    }
}

// 设置卡控件已缓存时原地刷；不再因按下推迟整卡 clean（±/翻页不拆树）
void ClearSettingsSheetWidgetRefs() {
    ResetReadingSheetLiveRefs();
    auto& st = Book_State();
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.settings.sheet_font_rows[i] = {};
    }
    st.settings.reader_sheet_body = nullptr;
    st.settings.reader_sheet_scroll = nullptr;
    st.settings.sheet_font_title = nullptr;
    st.settings.sheet_font_page_prev = nullptr;
    st.settings.sheet_font_page_next = nullptr;
    st.settings.sheet_font_page_lab = nullptr;
    st.settings.sheet_font_multi_bar = nullptr;
    st.settings.sheet_margin_value = nullptr;
    st.settings.sheet_margin_dec = nullptr;
    st.settings.sheet_margin_inc = nullptr;
    st.settings.sheet_margin_bound = nullptr;
    st.settings.sheet_gap_value = nullptr;
    st.settings.sheet_gap_dec = nullptr;
    st.settings.sheet_gap_inc = nullptr;
    st.settings.sheet_gap_bound = nullptr;
    st.settings.sheet_fl_bright_value = nullptr;
    st.ttf.sheet_font_import = nullptr;
    st.ttf.ttf_sheet = nullptr;
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf.ttf_rows[i] = {};
    }
    st.ttf.ttf_bar = nullptr;
    st.ttf.ttf_pct_lbl = nullptr;
    st.ttf.ttf_msg_lbl = nullptr;
    st.ttf.ttf_size_value = nullptr;
    st.ttf.ttf_size_dec = nullptr;
    st.ttf.ttf_size_inc = nullptr;
    st.tap.tap_zone_sheet = nullptr;
}

bool SettingsSheetWidgetsReady() {
    auto& st = Book_State();
    return st.settings.reader_sheet_body != nullptr && lv_obj_is_valid(st.settings.reader_sheet_body);
}

void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled) {
    if (btn == nullptr || !lv_obj_is_valid(btn)) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    }
}

void UpdateSheetBoundTip(lv_obj_t* tip, bool can_dec, bool can_inc) {
    if (tip == nullptr || !lv_obj_is_valid(tip)) {
        return;
    }
    if (!can_dec) {
        lv_label_set_text(tip, "MIN");
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_HIDDEN);
    } else if (!can_inc) {
        lv_label_set_text(tip, "MAX");
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(tip, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshSettingsSheet() {
    if (Book_State().reader.standby_paused) return;
    if (Book_State().settings.reader_sheet_scrolling) return;
    if (Book_State().reader.read_chrome == BookUiState::ReadChrome::kSettings && !RefreshReadingSheetInPlace()) RebuildSettingsSheet();
}

// 排版后保卡可见；控件已由 coalesce/Rebuild 就绪则不再叠刷
void EnsureSettingsSheetAfterLayout() {
    SetReadStatusVisible(true);
    SetReadSettingsSheetVisible(true);
    if (!SettingsSheetWidgetsReady()) {
        EnsureFontListPageShowsSelection();
        RebuildSettingsSheet();
    } else {
        RequestSettingsSheetPaint();
    }
}

void ShowReadSettingsSheet(bool default_duration) {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (ReaderPageImagesHint()) CancelPageImageLoad();
    if (default_duration) {
        // A fresh reader tap starts at duration, never resurrecting an import subpanel.
        TtfPanelClose();
        ResetBookmarkSelection();
        st.settings.reader_tab = 4;
    }
    if (st.settings.reader_tab == 1) StartReadFontScan();
    else CancelReadFontScan();
    if (st.settings.reader_tab == 0 || st.settings.reader_tab == 1)
        st.settings.reader_scroll[st.settings.reader_tab] = -1;
    TapZonesPanelClose();
    st.reader.read_chrome = BookUiState::ReadChrome::kSettings;
    SyncReaderAaForImmersion(true);
    SetReadStatusCenterTitle(nullptr);
    SetReadStatusVisible(true);
    SetReadSettingsSheetVisible(true);

    RebuildSettingsSheet();
}

void OpenReadToc() {
    ResetBookmarkSelection();
    Book_State().settings.reader_tab = 0;
    ShowReadSettingsSheet(false);
}

void OpenReadBookmarks() {
    Book_State().settings.reader_tab = 2;
    ShowReadSettingsSheet(false);
}

void OpenReadFontSettings() {
    ResetBookmarkSelection();
    Book_State().settings.reader_tab = 1;
    ShowReadSettingsSheet(false);
}

void OpenReadSettings() {
    ResetBookmarkSelection();
    Book_State().settings.reader_tab = 3;
    Book_State().settings.reader_settings_page = 0;
    ShowReadSettingsSheet(false);
}
