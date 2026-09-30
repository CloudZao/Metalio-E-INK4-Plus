#pragma GCC optimize("O1")

#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_text_util.h"
#include "book_screen/settings/book_ttf_actions.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <cstdio>

#include "assets/lang_config.h"
#include "haptic_feedback.h"

void SettingsSheetPopulateFont(BookUiState& st, lv_obj_t* parent) {
    const int font_i = CurrentFontIndex();
    const int font_n = static_cast<int>(st.settings.font_entries.size());
    const int font_pages = FontListPageCount();
    const int font_page_start = st.settings.font_list_page * kFontListPageSize;

// 字体：标题行（标题 + 导入 TTF）；多选时标题下多出操作行；底栏 ▲▼ 始终可翻页
lv_obj_t* font_title_row = lv_obj_create(parent);
lv_obj_remove_style_all(font_title_row);
lv_obj_set_width(font_title_row, lv_pct(100));
lv_obj_set_height(font_title_row, LV_SIZE_CONTENT);
lv_obj_set_flex_flow(font_title_row, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(font_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                      LV_FLEX_ALIGN_CENTER);
lv_obj_clear_flag(font_title_row, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(font_title_row);

lv_obj_t* font_title = lv_label_create(font_title_row);
lv_label_set_text(font_title, Lang::Strings::BOOK_FONT_TITLE);
lv_obj_set_style_text_font(font_title, Book_ItemFont(), 0);
lv_obj_set_style_text_opa(font_title, LV_OPA_70, 0);
lv_obj_clear_flag(font_title, LV_OBJ_FLAG_CLICKABLE);
st.settings.sheet_font_title = font_title;

lv_obj_t* import_btn = lv_obj_create(font_title_row);
lv_obj_remove_style_all(import_btn);
lv_obj_set_height(import_btn, 36);
lv_obj_set_width(import_btn, LV_SIZE_CONTENT);
lv_obj_set_style_bg_opa(import_btn, LV_OPA_TRANSP, 0);
lv_obj_set_style_pad_hor(import_btn, 4, 0);
lv_obj_set_flex_flow(import_btn, LV_FLEX_FLOW_COLUMN);
lv_obj_set_flex_align(import_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
Book_DisableScroll(import_btn);
lv_obj_add_flag(import_btn, LV_OBJ_FLAG_CLICKABLE);
HapticAttachClick(import_btn);
lv_obj_add_event_cb(import_btn, OnFontImportClick, LV_EVENT_CLICKED, nullptr);
lv_obj_add_event_cb(
    import_btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
lv_obj_t* import_wrap = lv_obj_create(import_btn);
lv_obj_remove_style_all(import_wrap);
lv_obj_set_size(import_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
lv_obj_set_style_border_side(import_wrap, LV_BORDER_SIDE_BOTTOM, 0);
lv_obj_set_style_border_width(import_wrap, kShelfUnderlineH, 0);
lv_obj_set_style_border_color(import_wrap, lv_color_black(), 0);
lv_obj_set_style_pad_bottom(import_wrap, 2, 0);
lv_obj_set_style_pad_hor(import_wrap, kShelfUnderlinePadHor, 0);
lv_obj_set_style_bg_opa(import_wrap, LV_OPA_TRANSP, 0);
Book_DisableScroll(import_wrap);
lv_obj_clear_flag(import_wrap, LV_OBJ_FLAG_CLICKABLE);
lv_obj_t* import_lbl = lv_label_create(import_wrap);
lv_label_set_text(import_lbl, "导入 TTF");
lv_obj_set_style_text_font(import_lbl, Book_ItemFont(), 0);
lv_obj_set_style_text_color(import_lbl, lv_color_black(), 0);
lv_obj_clear_flag(import_lbl, LV_OBJ_FLAG_CLICKABLE);
st.ttf.sheet_font_import = import_btn;

st.settings.sheet_font_multi_bar = lv_obj_create(parent);
lv_obj_remove_style_all(st.settings.sheet_font_multi_bar);
lv_obj_set_width(st.settings.sheet_font_multi_bar, lv_pct(100));
lv_obj_set_height(st.settings.sheet_font_multi_bar, 36);
lv_obj_set_style_bg_opa(st.settings.sheet_font_multi_bar, LV_OPA_TRANSP, 0);
lv_obj_set_flex_flow(st.settings.sheet_font_multi_bar, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(st.settings.sheet_font_multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                      LV_FLEX_ALIGN_CENTER);
lv_obj_set_style_pad_column(st.settings.sheet_font_multi_bar, 10, 0);
Book_DisableScroll(st.settings.sheet_font_multi_bar);
lv_obj_clear_flag(st.settings.sheet_font_multi_bar, LV_OBJ_FLAG_CLICKABLE);
lv_obj_add_flag(st.settings.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);

auto make_font_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, 36);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(btn, 4, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* text_wrap = lv_obj_create(btn);
    lv_obj_remove_style_all(text_wrap);
    lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(text_wrap, kShelfUnderlineH, 0);
    lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
    lv_obj_set_style_pad_hor(text_wrap, kShelfUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
    Book_DisableScroll(text_wrap);
    lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(text_wrap);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
};
auto make_font_multi_dot = [](lv_obj_t* parent) {
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, kShelfMultiDotSize, kShelfMultiDotSize);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    Book_DisableScroll(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return dot;
};
make_font_multi_action(st.settings.sheet_font_multi_bar, Lang::Strings::COMMON_CANCEL, OnFontMultiCancel);
make_font_multi_dot(st.settings.sheet_font_multi_bar);
make_font_multi_action(st.settings.sheet_font_multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnFontMultiSelectAll);
make_font_multi_dot(st.settings.sheet_font_multi_bar);
make_font_multi_action(st.settings.sheet_font_multi_bar, Lang::Strings::COMMON_REMOVE, OnFontMultiRemove);

constexpr lv_coord_t kFontListH = kFontListRowH * kFontListPageSize;
lv_obj_t* font_panel = lv_obj_create(parent);
lv_obj_remove_style_all(font_panel);
lv_obj_set_width(font_panel, lv_pct(100));
lv_obj_set_height(font_panel, kFontListH + 40);  // 列表 + 翻页条
lv_obj_set_style_bg_color(font_panel, lv_color_white(), 0);
lv_obj_set_style_bg_opa(font_panel, LV_OPA_COVER, 0);
lv_obj_set_style_border_width(font_panel, kSheetBorderW, 0);
lv_obj_set_style_border_color(font_panel, lv_color_black(), 0);
lv_obj_set_style_radius(font_panel, 12, 0);
lv_obj_set_flex_flow(font_panel, LV_FLEX_FLOW_COLUMN);
lv_obj_clear_flag(font_panel, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(font_panel);

lv_obj_t* font_list = lv_obj_create(font_panel);
lv_obj_remove_style_all(font_list);
lv_obj_set_width(font_list, lv_pct(100));
lv_obj_set_height(font_list, kFontListH);
lv_obj_set_flex_flow(font_list, LV_FLEX_FLOW_COLUMN);
lv_obj_clear_flag(font_list, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(font_list);

if (font_n <= 0) {
    lv_obj_t* empty = lv_label_create(font_list);
    lv_label_set_text(empty, Lang::Strings::BOOK_FONT_EMPTY);
    lv_obj_set_style_text_font(empty, Book_ItemFont(), 0);
    lv_obj_set_style_pad_all(empty, 12, 0);
    lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
} else {
    for (int i = 0; i < kFontListPageSize; ++i) {
        const int idx = font_page_start + i;
        lv_obj_t* row = lv_obj_create(font_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kFontListRowH);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        Book_DisableScroll(row);

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* px = lv_label_create(row);
        lv_obj_set_style_text_font(px, Book_ItemFont(), 0);
        lv_obj_clear_flag(px, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kShelfCheckSize, kShelfCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, kRowBorderW, 0);
        lv_obj_set_style_radius(check, 4, 0);
        Book_DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);

        st.settings.sheet_font_rows[i].row = row;
        st.settings.sheet_font_rows[i].name = name;
        st.settings.sheet_font_rows[i].px = px;
        st.settings.sheet_font_rows[i].check = check;

        // 始终挂选中；空槽由 Refresh / user_data=-1 拒点
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, OnSheetFontSelect, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(row, OnSheetFontLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_add_event_cb(
            row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);

        if (idx >= font_n) {
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
            lv_label_set_text(name, "");
            lv_label_set_text(px, "");
            continue;
        }

        const ReadFontEntry& fe = st.settings.font_entries[static_cast<size_t>(idx)];
        const bool on = (!st.settings.font_multi && idx == font_i);
        const bool next_on = (!st.settings.font_multi) && (i + 1 < kFontListPageSize) &&
                             (font_page_start + i + 1 == font_i) &&
                             (font_page_start + i + 1 < font_n);
        // 选中：上下加粗横线（同目录当前章）；上一行勿叠细底线
        if (on) {
            lv_obj_set_style_border_width(row, kTocLineCur, 0);
            lv_obj_set_style_border_side(
                row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                0);
        } else if (next_on) {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_NONE, 0);
        } else if (i + 1 < kFontListPageSize) {
            lv_obj_set_style_border_width(row, kTocLineThin, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
        }
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

        char name_buf[40];
        FormatFontListName(fe.file, name_buf, sizeof(name_buf));
        lv_label_set_text(name, name_buf);
        lv_obj_set_style_text_font(name, on ? Book_ListFont() : Book_ItemFont(), 0);

        char px_buf[16];
        if (fe.size_px > 0) {
            std::snprintf(px_buf, sizeof(px_buf), "%dpx", fe.size_px);
        } else {
            std::snprintf(px_buf, sizeof(px_buf), "--");
        }
        lv_label_set_text(px, px_buf);
        lv_obj_set_style_text_opa(px, on ? LV_OPA_COVER : LV_OPA_70, 0);

        if (st.settings.font_multi) {
            lv_obj_clear_flag(check, LV_OBJ_FLAG_HIDDEN);
            if (FontItemSelected(idx)) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, Book_ItemFont(), 0);
                lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                lv_obj_center(mark);
                lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
}

lv_obj_t* pager = lv_obj_create(font_panel);
lv_obj_remove_style_all(pager);
lv_obj_set_width(pager, lv_pct(100));
lv_obj_set_height(pager, 40);
lv_obj_set_style_pad_hor(pager, 8, 0);
lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
lv_obj_set_style_border_width(pager, kTocLineThin, 0);
lv_obj_set_style_border_side(pager, LV_BORDER_SIDE_TOP, 0);
lv_obj_set_style_border_color(pager, lv_color_black(), 0);
lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                      LV_FLEX_ALIGN_CENTER);
lv_obj_clear_flag(pager, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(pager);

const bool can_prev = st.settings.font_list_page > 0;
const bool can_next = st.settings.font_list_page + 1 < font_pages;
auto make_page_btn = [&](const char* txt, lv_event_cb_t cb, bool enabled) -> lv_obj_t* {
    lv_obj_t* btn = lv_obj_create(pager);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kFontPageBtnW, kFontPageBtnH);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    Book_DisableScroll(btn);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    SetSheetBtnEnabled(btn, enabled);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
};
st.settings.sheet_font_page_prev = make_page_btn("▲", OnSheetFontPagePrev, can_prev);
char page_meta[24];
std::snprintf(page_meta, sizeof(page_meta), "%d / %d", st.settings.font_list_page + 1, font_pages);
lv_obj_t* page_lab = lv_label_create(pager);
lv_label_set_text(page_lab, page_meta);
lv_obj_set_style_text_font(page_lab, Book_ItemFont(), 0);
lv_obj_set_style_text_opa(page_lab, LV_OPA_70, 0);
lv_obj_clear_flag(page_lab, LV_OBJ_FLAG_CLICKABLE);
st.settings.sheet_font_page_lab = page_lab;
st.settings.sheet_font_page_next = make_page_btn("▼", OnSheetFontPageNext, can_next);
RefreshFontMultiFooter();

}

