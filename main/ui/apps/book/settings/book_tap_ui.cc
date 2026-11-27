#include "assets/lang_reader.h"


#include <algorithm>
#include <cstdint>
#include <lvgl.h>
#include "ui/apps/book/settings/book_tap_ui.h"
#include "assets/lang_config.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

const char* TapZoneActionLabel(BookTapZoneAction action) {
    switch (action) {
        case BookTapZoneAction::kPrev:
            return LangString_BOOK_TAP_ZONE_PREV;
        case BookTapZoneAction::kMenu:
            return LangString_BOOK_TAP_ZONE_MENU;
        case BookTapZoneAction::kNext:
            return LangString_BOOK_TAP_ZONE_NEXT;
        case BookTapZoneAction::kBack:
            return LangString_BOOK_TAP_ZONE_BACK;
        case BookTapZoneAction::kToc:
            return LangString_BOOK_TOC;
        case BookTapZoneAction::kBookmarks:
            return LangString_BOOK_BOOKMARKS;
        case BookTapZoneAction::kFontSettings:
            return LangString_BOOK_FONT_TITLE;
        case BookTapZoneAction::kSettings:
            return LangString_HOME_APP_SETTINGS;
        case BookTapZoneAction::kFullRefresh:
            return LangString_BOOK_TAP_ZONE_FULL_REFRESH;
        case BookTapZoneAction::kNone:
        default:
            return LangString_BOOK_TAP_ZONE_NONE;
    }
}

bool TapZonePickOpen() {
    const int cell = Book_State().tap.tap_zone_pick_cell;
    return cell >= 0 && cell < kBookTapZoneCellCount;
}

static void OnTapZoneCellClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int cell = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        return;
    }
    Book_State().tap.tap_zone_pick_cell = cell;
    RebuildSettingsSheet();
}

static void OnTapZoneActionPicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    const int cell = st.tap.tap_zone_pick_cell;
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        st.tap.tap_zone_pick_cell = -1;
        RebuildSettingsSheet();
        return;
    }
    const auto action =
        static_cast<BookTapZoneAction>(static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    if (static_cast<uint8_t>(action) >= static_cast<uint8_t>(BookTapZoneAction::kCount)) {
        return;
    }
    if (!BookTapZonesSetAction(cell, action)) {
        ScreenShowToast(LangString_BOOK_TAP_MENU_REQUIRED);
        return;
    }
    st.tap.tap_zone_pick_cell = -1;
    RebuildSettingsSheet();
}

void OnTapZoneReset(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookTapZonesResetDefaults();
    Book_State().tap.tap_zone_pick_cell = -1;
    RebuildSettingsSheet();
}

void BuildTapZoneSettings(lv_obj_t* body, int content_h) {
    auto* host = lv_obj_create(body);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, lv_pct(100), std::max(1, content_h));
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_COLUMN);
    Book_DisableScroll(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);

    if (TapZonePickOpen()) {
        const int cell = Book_State().tap.tap_zone_pick_cell;
        const BookTapZoneAction cur = BookTapZonesActionAt(cell);
        constexpr BookTapZoneAction kChoices[] = {
            BookTapZoneAction::kPrev,         BookTapZoneAction::kMenu,     BookTapZoneAction::kNext,
            BookTapZoneAction::kBack,         BookTapZoneAction::kToc,      BookTapZoneAction::kBookmarks,
            BookTapZoneAction::kFontSettings, BookTapZoneAction::kSettings, BookTapZoneAction::kFullRefresh,
            BookTapZoneAction::kNone,
        };
        auto add_choice = [&](lv_obj_t* parent, BookTapZoneAction a) {
            const bool selected = (a == cur);
            lv_obj_t* row = lv_obj_create(parent);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, 0);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, UiColorForeground(), 0);
            lv_obj_set_style_bg_color(row, selected ? UiColorForeground() : UiColorBackground(), 0);
            lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_hor(row, 8, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            Book_DisableScroll(row);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(row);
            lv_obj_add_event_cb(row, OnTapZoneActionPicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(a)));
            lv_obj_add_event_cb(row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* name = lv_label_create(row);
            lv_label_set_text(name, TapZoneActionLabel(a));
            lv_obj_set_style_text_font(name, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(name, selected ? UiColorBackground() : UiColorForeground(), 0);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_update_layout(row);
            ScreenFitLabel(name, lv_obj_get_content_width(row), std::max<int>(1, lv_obj_get_content_height(row)));
        };
        const int choice_n = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));
        lv_obj_t* cols = lv_obj_create(host);
        lv_obj_remove_style_all(cols);
        lv_obj_set_width(cols, lv_pct(100));
        lv_obj_set_flex_grow(cols, 1);
        lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(cols, 10, 0);
        lv_obj_clear_flag(cols, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(cols);
        auto make_col = [&]() {
            lv_obj_t* col = lv_obj_create(cols);
            lv_obj_remove_style_all(col);
            lv_obj_set_height(col, lv_pct(100));
            lv_obj_set_flex_grow(col, 1);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(col, 6, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(col);
            return col;
        };
        lv_obj_t* left = make_col();
        lv_obj_t* right = make_col();
        const int mid = (choice_n + 1) / 2;
        for (int i = 0; i < mid; ++i) {
            add_choice(left, kChoices[i]);
        }
        for (int i = mid; i < choice_n; ++i) {
            add_choice(right, kChoices[i]);
        }
        return;
    }

    lv_obj_t* grid = lv_obj_create(host);
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
            const int zone = r * 3 + c;
            lv_obj_t* cell_obj = lv_obj_create(row);
            lv_obj_remove_style_all(cell_obj);
            lv_obj_set_flex_grow(cell_obj, 1);
            lv_obj_set_height(cell_obj, lv_pct(100));
            lv_obj_set_style_border_width(cell_obj, 2, 0);
            lv_obj_set_style_border_color(cell_obj, UiColorForeground(), 0);
            lv_obj_set_style_radius(cell_obj, 4, 0);
            lv_obj_add_flag(cell_obj, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(cell_obj);
            HapticAttachClick(cell_obj);
            lv_obj_add_event_cb(cell_obj, OnTapZoneCellClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(zone)));
            lv_obj_add_event_cb(
                cell_obj, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* lab = lv_label_create(cell_obj);
            lv_label_set_text(lab, TapZoneActionLabel(BookTapZonesActionAt(zone)));
            lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
            lv_obj_center(lab);
            lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}
