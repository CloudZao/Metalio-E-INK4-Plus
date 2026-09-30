#pragma GCC optimize("O1")

#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <cstdio>

void SetReadStatusVisible(bool visible) {
    auto& st = Book_State();
    if (st.reader.status_bar != nullptr && lv_obj_is_valid(st.reader.status_bar)) {
        if (visible) {
            // 盖在正文上时需白底，否则字迹透出
            lv_obj_set_style_bg_color(st.reader.status_bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(st.reader.status_bar, LV_OPA_COVER, 0);
            lv_obj_clear_flag(st.reader.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.reader.status_bar);
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
            lv_obj_move_foreground(st.reader.status_overlay);
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
    auto& st = Book_State();
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    if (visible) {
        ApplyReaderChromeSize();
        lv_obj_clear_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(st.settings.settings_sheet);
        if (st.reader.status_bar != nullptr && lv_obj_is_valid(st.reader.status_bar)) {
            lv_obj_move_foreground(st.reader.status_bar);
        }
        if (st.reader.status_overlay != nullptr && lv_obj_is_valid(st.reader.status_overlay)) {
            lv_obj_move_foreground(st.reader.status_overlay);
        }
    } else {
        lv_obj_add_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

void HideReadChrome() {
    auto& st = Book_State();
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
    TapZonesPanelClose();
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
        // 关卡：落盘未完成的 ± 排版，并重绘正文
        FlushLayoutDebounce();
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        StartChapterPageWorker();
        if (st.reader.session && st.reader.session->IsOpen() && st.reader.content != nullptr) {
            RenderReaderPage();
        }
    }
}

// 设置卡控件已缓存时原地刷；不再因按下推迟整卡 clean（±/翻页不拆树）
void ClearSettingsSheetWidgetRefs() {
    auto& st = Book_State();
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.settings.sheet_font_rows[i] = {};
    }
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
    return st.settings.settings_sheet != nullptr && lv_obj_is_valid(st.settings.settings_sheet) &&
           st.settings.sheet_margin_value != nullptr && lv_obj_is_valid(st.settings.sheet_margin_value) &&
           st.settings.sheet_font_page_lab != nullptr && lv_obj_is_valid(st.settings.sheet_font_page_lab);
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
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
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

void RefreshSettingsSheetAdj() {
    auto& st = Book_State();
    if (!SettingsSheetWidgetsReady()) {
        return;
    }
    const int margin_i = BookReaderPrefsMarginPreset();
    const int gap_i = BookReaderPrefsSpacingPreset();
    const bool margin_dec = margin_i > 0;
    const bool margin_inc = margin_i + 1 < kBookReaderMarginPresetCount;
    const bool gap_dec = gap_i > 0;
    const bool gap_inc = gap_i + 1 < kBookReaderSpacingPresetCount;

    lv_label_set_text(st.settings.sheet_margin_value, BookReaderPrefsMarginLabel(margin_i));
    if (st.settings.sheet_gap_value != nullptr && lv_obj_is_valid(st.settings.sheet_gap_value)) {
        lv_label_set_text(st.settings.sheet_gap_value, BookReaderPrefsSpacingLabel(gap_i));
    }
    SetSheetBtnEnabled(st.settings.sheet_margin_dec, margin_dec);
    SetSheetBtnEnabled(st.settings.sheet_margin_inc, margin_inc);
    SetSheetBtnEnabled(st.settings.sheet_gap_dec, gap_dec);
    SetSheetBtnEnabled(st.settings.sheet_gap_inc, gap_inc);
    UpdateSheetBoundTip(st.settings.sheet_margin_bound, margin_dec, margin_inc);
    UpdateSheetBoundTip(st.settings.sheet_gap_bound, gap_dec, gap_inc);
}

void RefreshSettingsSheetFontList() {
    auto& st = Book_State();
    if (!SettingsSheetWidgetsReady()) {
        return;
    }
    ClampFontListPage();
    SyncFontSelectedSize();
    const int font_i = CurrentFontIndex();
    const int font_n = static_cast<int>(st.settings.font_entries.size());
    const int font_pages = FontListPageCount();
    const int font_page_start = st.settings.font_list_page * kFontListPageSize;

    for (int i = 0; i < kFontListPageSize; ++i) {
        auto& fr = st.settings.sheet_font_rows[i];
        if (fr.row == nullptr || !lv_obj_is_valid(fr.row)) {
            continue;
        }
        const int idx = font_page_start + i;
        if (idx >= font_n) {
            lv_obj_clear_flag(fr.row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
            lv_obj_set_user_data(fr.row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
            if (fr.name != nullptr && lv_obj_is_valid(fr.name)) {
                lv_label_set_text(fr.name, "");
            }
            if (fr.px != nullptr && lv_obj_is_valid(fr.px)) {
                lv_label_set_text(fr.px, "");
            }
            if (fr.check != nullptr && lv_obj_is_valid(fr.check)) {
                lv_obj_add_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
            }
            continue;
        }

        const ReadFontEntry& fe = st.settings.font_entries[static_cast<size_t>(idx)];
        const bool on = (!st.settings.font_multi && idx == font_i);
        const bool next_on = (!st.settings.font_multi) && (i + 1 < kFontListPageSize) &&
                             (font_page_start + i + 1 == font_i) &&
                             (font_page_start + i + 1 < font_n);
        if (on) {
            lv_obj_set_style_border_width(fr.row, kTocLineCur, 0);
            lv_obj_set_style_border_side(
                fr.row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                0);
        } else if (next_on) {
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
        } else if (i + 1 < kFontListPageSize) {
            lv_obj_set_style_border_width(fr.row, kTocLineThin, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_BOTTOM, 0);
        } else {
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
        }
        lv_obj_set_style_border_color(fr.row, lv_color_black(), 0);
        lv_obj_set_style_border_opa(fr.row, LV_OPA_COVER, 0);

        lv_obj_add_flag(fr.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(fr.row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

        char name_buf[40];
        FormatFontListName(fe.file, name_buf, sizeof(name_buf));
        if (fr.name != nullptr && lv_obj_is_valid(fr.name)) {
            lv_label_set_text(fr.name, name_buf);
            lv_obj_set_style_text_font(fr.name, on ? Book_ListFont() : Book_ItemFont(), 0);
        }
        char px_buf[16];
        if (fe.size_px > 0) {
            std::snprintf(px_buf, sizeof(px_buf), "%dpx", fe.size_px);
        } else {
            std::snprintf(px_buf, sizeof(px_buf), "--");
        }
        if (fr.px != nullptr && lv_obj_is_valid(fr.px)) {
            lv_label_set_text(fr.px, px_buf);
            lv_obj_set_style_text_opa(fr.px, on ? LV_OPA_COVER : LV_OPA_70, 0);
        }
        if (fr.check != nullptr && lv_obj_is_valid(fr.check)) {
            if (st.settings.font_multi) {
                lv_obj_clear_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
                if (FontItemSelected(idx)) {
                    lv_obj_t* mark = lv_label_create(fr.check);
                    lv_label_set_text(mark, "√");
                    lv_obj_set_style_text_font(mark, Book_ItemFont(), 0);
                    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                    lv_obj_center(mark);
                    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
                }
            } else {
                lv_obj_add_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
            }
        }
    }

    const bool can_prev = st.settings.font_list_page > 0;
    const bool can_next = st.settings.font_list_page + 1 < font_pages;
    SetSheetBtnEnabled(st.settings.sheet_font_page_prev, can_prev);
    SetSheetBtnEnabled(st.settings.sheet_font_page_next, can_next);
    if (st.settings.sheet_font_page_lab != nullptr && lv_obj_is_valid(st.settings.sheet_font_page_lab)) {
        char page_meta[24];
        std::snprintf(page_meta, sizeof(page_meta), "%d / %d", st.settings.font_list_page + 1, font_pages);
        lv_label_set_text(st.settings.sheet_font_page_lab, page_meta);
    }
    RefreshFontMultiFooter();
}

void RefreshSettingsSheet() {
    if (Book_State().reader.read_chrome != BookUiState::ReadChrome::kSettings) {
        return;
    }
    RefreshSettingsSheetFontList();
    RefreshSettingsSheetAdj();
}

// 排版后保卡可见；控件已由 coalesce/Rebuild 就绪则不再叠刷
void EnsureSettingsSheetAfterLayout() {
    SetReadStatusVisible(true);
    SetReadSettingsSheetVisible(true);
    if (!SettingsSheetWidgetsReady()) {
        EnsureFontListPageShowsSelection();
        RebuildSettingsSheet();
    }
}

void ShowReadSettingsSheet() {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    TapZonesPanelClose();
    s_reader_page_delta.store(0, std::memory_order_release);
    st.reader.read_chrome = BookUiState::ReadChrome::kSettings;
    SyncReaderAaForImmersion(false);
    SetReadStatusCenterTitle(nullptr);
    SetReadStatusVisible(true);
    SetReadSettingsSheetVisible(true);

    EnsureFontListPageShowsSelection();
    if (SettingsSheetWidgetsReady()) {
        // 与 ▲▼/± 同路：合并上屏，勿同步再刷一帧
        RequestSettingsSheetPaint();
        return;
    }
    RebuildSettingsSheet();
}

void OpenReadToc() {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    FlushLayoutDebounce();
    TapZonesPanelClose();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        return;
    }
    s_reader_page_delta.store(0, std::memory_order_release);
    SetReadSettingsSheetVisible(false);
    st.reader.read_chrome = BookUiState::ReadChrome::kToc;
    SyncReaderAaForImmersion(false);
    // -1：等 RenderTocList 算出本页行数后再落到当前章所在页
    st.reader.toc_list_page = -1;
    ApplyReaderPageGeometry(ReaderGeomMode::kOverlayList);
    RenderTocList();
}

