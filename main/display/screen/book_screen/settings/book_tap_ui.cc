#pragma GCC optimize("O1")

#include "book_screen/settings/book_tap_ui.h"
#include "screen_common.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/book_text_util.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/reader/book_reader_overlay.h"

#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <cstdint>

#include "assets/lang_config.h"
#include "display/font/ttf_convert.h"
#include "display_orient.h"
#include "haptic_feedback.h"

const char* TapZoneActionLabel(BookTapZoneAction action) {
    switch (action) {
        case BookTapZoneAction::kPrev:
            return Lang::Strings::BOOK_TAP_ZONE_PREV;
        case BookTapZoneAction::kMenu:
            return Lang::Strings::BOOK_TAP_ZONE_MENU;
        case BookTapZoneAction::kNext:
            return Lang::Strings::BOOK_TAP_ZONE_NEXT;
        case BookTapZoneAction::kBack:
            return Lang::Strings::BOOK_TAP_ZONE_BACK;
        case BookTapZoneAction::kToc:
            return Lang::Strings::BOOK_TOC;
        case BookTapZoneAction::kBookmarks:
            return "书签";
        case BookTapZoneAction::kFontSettings:
            return "字体";
        case BookTapZoneAction::kSettings:
            return "设置";
        case BookTapZoneAction::kFullRefresh:
            return Lang::Strings::BOOK_TAP_ZONE_FULL_REFRESH;
        case BookTapZoneAction::kNone:
        default:
            return Lang::Strings::BOOK_TAP_ZONE_NONE;
    }
}
void TapZonesPanelClose() {
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet != nullptr && lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        lv_obj_del(st.tap.tap_zone_sheet);
    }
    st.tap.tap_zone_sheet = nullptr;
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kClosed;
    st.tap.tap_zone_pick_cell = -1;
}

void TapZonesRebuildContent() {
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet == nullptr || !lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        return;
    }
    lv_obj_clean(st.tap.tap_zone_sheet);
    ApplyReadingPanelRules(st.tap.tap_zone_sheet);

    auto make_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                          intptr_t user) -> lv_obj_t* {
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
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(user));
        lv_obj_add_event_cb(
            btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(wrap, 2, 0);
        lv_obj_set_style_pad_hor(wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        Book_DisableScroll(wrap);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* lbl = lv_label_create(wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };

    const bool picking = (st.tap.tap_zone_ui == BookUiState::TapZoneUi::kPick);
    auto* head = lv_obj_create(st.tap.tap_zone_sheet);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 52);
    Book_DisableScroll(head);
    auto* back = make_action(head, "<", picking ? OnTapZonePickBack : OnTapZoneClose, 0);
    lv_obj_set_style_border_width(lv_obj_get_child(back, 0), 0, 0);
    lv_obj_set_style_pad_all(lv_obj_get_child(back, 0), 0, 0);
    lv_obj_set_size(back, 48, 48);
    lv_obj_set_ext_click_area(back, 12);
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(back, 0), 0), "");
    static const lv_point_precise_t left_points[] = {{10, 0}, {0, 12}, {10, 24}};
    auto* left = lv_line_create(back);
    lv_obj_add_flag(left, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_line_set_points(left, left_points, 3);
    lv_obj_set_style_line_width(left, 4, 0);
    lv_obj_set_style_line_color(left, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(left, true, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(left);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* title = lv_label_create(head);
    lv_label_set_text(title, picking ? "选择动作" : "触摸分区");
    lv_obj_set_style_text_font(title, Book_ListFont(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    if (!picking) {
        auto* reset = make_action(head, "恢复默认", OnTapZoneReset, 0);
        // 边框设在固定高度按钮上，避免子对象超高时被裁剪。
        lv_obj_set_height(reset, 42);
        lv_obj_set_style_border_width(reset, 1, 0);
        lv_obj_set_style_border_color(reset, lv_color_black(), 0);
        lv_obj_set_style_radius(reset, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(reset, 12, 0);
        auto* wrap = lv_obj_get_child(reset, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_align(reset, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    if (picking) {
        const int cell = st.tap.tap_zone_pick_cell;
        const BookTapZoneAction cur =
            (cell >= 0 && cell < kBookTapZoneCellCount) ? BookTapZonesActionAt(cell)
                                                        : BookTapZoneAction::kNone;
        constexpr BookTapZoneAction kChoices[] = {
            BookTapZoneAction::kPrev,         BookTapZoneAction::kMenu,
            BookTapZoneAction::kNext,         BookTapZoneAction::kBack,
            BookTapZoneAction::kToc,          BookTapZoneAction::kBookmarks,
            BookTapZoneAction::kFontSettings, BookTapZoneAction::kSettings,
            BookTapZoneAction::kFullRefresh,
            BookTapZoneAction::kNone,
        };
        auto add_choice = [&](lv_obj_t* parent, BookTapZoneAction a) {
            const bool selected = (a == cur);
            lv_obj_t* row = lv_obj_create(parent);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, kTapZoneRowH);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, lv_color_black(), 0);
            lv_obj_set_style_bg_color(row, selected ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_hor(row, 8, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            Book_DisableScroll(row);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(row);
            lv_obj_add_event_cb(row, OnTapZoneActionPicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(a)));
            lv_obj_add_event_cb(
                row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* name = lv_label_create(row);
            lv_label_set_text(name, TapZoneActionLabel(a));
            lv_obj_set_style_text_font(name, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(name, selected ? lv_color_white() : lv_color_black(), 0);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        };
        const int choice_n = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));
        if (DisplayUiIsLandscape()) {
            lv_obj_t* cols = lv_obj_create(st.tap.tap_zone_sheet);
            lv_obj_remove_style_all(cols);
            lv_obj_set_width(cols, lv_pct(100));
            lv_obj_set_height(cols, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_column(cols, 10, 0);
            lv_obj_clear_flag(cols, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(cols);
            auto make_col = [&]() {
                lv_obj_t* col = lv_obj_create(cols);
                lv_obj_remove_style_all(col);
                lv_obj_set_height(col, LV_SIZE_CONTENT);
                lv_obj_set_flex_grow(col, 1);
                lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_style_pad_row(col, 6, 0);
                lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
                Book_DisableScroll(col);
                return col;
            };
            lv_obj_t* left = make_col();
            lv_obj_t* rule = Book_AddColRule(cols);
            lv_obj_t* right = make_col();
            const int mid = (choice_n + 1) / 2;
            for (int i = 0; i < mid; ++i) {
                add_choice(left, kChoices[i]);
            }
            for (int i = mid; i < choice_n; ++i) {
                add_choice(right, kChoices[i]);
            }
            Book_FinishColRule(cols, rule);
        } else {
            auto* choices = lv_obj_create(st.tap.tap_zone_sheet);
            lv_obj_remove_style_all(choices);
            lv_obj_set_width(choices, lv_pct(100));
            lv_obj_set_height(choices, 0);
            lv_obj_set_flex_grow(choices, 1);
            lv_obj_set_flex_flow(choices, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(choices, 6, 0);
            lv_obj_add_flag(choices, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(choices, LV_OBJ_FLAG_SCROLL_ELASTIC);
            lv_obj_clear_flag(choices, LV_OBJ_FLAG_SCROLL_MOMENTUM);
            lv_obj_set_scroll_dir(choices, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(choices, LV_SCROLLBAR_MODE_AUTO);
            for (int i = 0; i < choice_n; ++i) {
                add_choice(choices, kChoices[i]);
            }
        }
        ApplyReadingPanelRules(st.tap.tap_zone_sheet);
        return;
    }

    lv_obj_t* grid = lv_obj_create(st.tap.tap_zone_sheet);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 6, 0);
    Book_DisableScroll(grid);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE);

    for (int r = 0; r < 3; ++r) {
        lv_obj_t* row = lv_obj_create(grid);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 6, 0);
        Book_DisableScroll(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        for (int c = 0; c < 3; ++c) {
            const int cell = r * 3 + c;
            lv_obj_t* cell_obj = lv_obj_create(row);
            lv_obj_remove_style_all(cell_obj);
            lv_obj_set_flex_grow(cell_obj, 1);
            lv_obj_set_height(cell_obj, lv_pct(100));
            lv_obj_set_style_border_width(cell_obj, 2, 0);
            lv_obj_set_style_border_color(cell_obj, lv_color_black(), 0);
            lv_obj_set_style_radius(cell_obj, 4, 0);
            lv_obj_add_flag(cell_obj, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(cell_obj);
            HapticAttachClick(cell_obj);
            lv_obj_add_event_cb(cell_obj, OnTapZoneCellClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(cell)));
            lv_obj_add_event_cb(
                cell_obj, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED,
                nullptr);
            lv_obj_t* lab = lv_label_create(cell_obj);
            lv_label_set_text(lab, TapZoneActionLabel(BookTapZonesActionAt(cell)));
            lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
            lv_obj_center(lab);
            lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    ApplyReadingPanelRules(st.tap.tap_zone_sheet);
}

void TapZonesEnsurePanel() {
    auto& st = Book_State();
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    if (st.tap.tap_zone_ui == BookUiState::TapZoneUi::kClosed) {
        st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    }
    lv_obj_update_layout(st.settings.settings_sheet);
    if (st.tap.tap_zone_sheet != nullptr && lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        lv_obj_set_size(st.tap.tap_zone_sheet, lv_pct(100), lv_pct(100));
        lv_obj_align(st.tap.tap_zone_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_move_foreground(st.tap.tap_zone_sheet);
        TapZonesRebuildContent();
        return;
    }
    st.tap.tap_zone_sheet = lv_obj_create(st.settings.settings_sheet);
    lv_obj_remove_style_all(st.tap.tap_zone_sheet);
    lv_obj_add_flag(st.tap.tap_zone_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(st.tap.tap_zone_sheet, lv_pct(100), lv_pct(100));
    lv_obj_align(st.tap.tap_zone_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(st.tap.tap_zone_sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(st.tap.tap_zone_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st.tap.tap_zone_sheet, 12, 0);
    lv_obj_set_style_pad_all(st.tap.tap_zone_sheet, 16, 0);
    lv_obj_set_style_pad_row(st.tap.tap_zone_sheet, 18, 0);
    lv_obj_set_flex_flow(st.tap.tap_zone_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(st.tap.tap_zone_sheet, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(st.tap.tap_zone_sheet);
    lv_obj_move_foreground(st.tap.tap_zone_sheet);
    TapZonesRebuildContent();
}

void OnSheetTapZonesClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    // 与 TTF 浮层互斥：转换中勿打断；其它态先收起再进分区配置
    if (ttf_convert::Busy() || st.ttf.ttf_mode == BookUiState::TtfMode::kProgress) {
        return;
    }
    TtfPanelClose();
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    st.tap.tap_zone_pick_cell = -1;
    TapZonesEnsurePanel();
}

void OnTapZoneCellClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet == nullptr || !lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        return;
    }
    const int cell = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        return;
    }
    st.tap.tap_zone_pick_cell = cell;
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kPick;
    TapZonesRebuildContent();
}

void OnTapZoneActionPicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet == nullptr || !lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        return;
    }
    const int cell = st.tap.tap_zone_pick_cell;
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
        st.tap.tap_zone_pick_cell = -1;
        TapZonesRebuildContent();
        return;
    }
    const auto action = static_cast<BookTapZoneAction>(
        static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    if (static_cast<uint8_t>(action) >= static_cast<uint8_t>(BookTapZoneAction::kCount)) {
        return;
    }
    if (!BookTapZonesSetAction(cell, action)) {
        ScreenShowToast("至少保留一个菜单唤醒区域");
        return;
    }
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    st.tap.tap_zone_pick_cell = -1;
    TapZonesRebuildContent();
}

void OnTapZoneReset(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet == nullptr || !lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        return;
    }
    BookTapZonesResetDefaults();
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    st.tap.tap_zone_pick_cell = -1;
    TapZonesRebuildContent();
}

void OnTapZoneClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    TapZonesPanelClose();
}

void OnTapZonePickBack(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.tap.tap_zone_sheet == nullptr || !lv_obj_is_valid(st.tap.tap_zone_sheet)) {
        return;
    }
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    st.tap.tap_zone_pick_cell = -1;
    TapZonesRebuildContent();
}

