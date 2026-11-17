#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <new>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "reader/reader.h"

void RequestShelfCheckMarksPaint() {
    if (s_shelf_check_paint.paint == nullptr) {
        s_shelf_check_paint.paint = PatchShelfVisibleCheckMarks;
    }
    ScreenPaintCoalesceRequestDebounced(&s_shelf_check_paint, kShelfCheckPaintDebounceUs);
}

int ShelfSelectedCount() {
    return static_cast<int>(Book_State().shelf.shelf_selected.size());
}

bool ShelfItemSelected(int idx) {
    const auto& shelf = Book_State().shelf;
    if (idx < 0 || idx >= static_cast<int>(shelf.books.size())) {
        return false;
    }
    const auto& path = shelf.books[idx].path;
    return std::any_of(shelf.shelf_selected.begin(), shelf.shelf_selected.end(),
                       [&](const reader::BookDirEntry& entry) { return entry.path == path; });
}

void ToggleShelfItemSelected(int idx) try {
    auto& shelf = Book_State().shelf;
    if (ShelfDeleteBusy() || shelf.directory_loading || idx < 0 || idx >= static_cast<int>(shelf.books.size())) {
        return;
    }
    const auto& book = shelf.books[idx];
    const auto found = std::find_if(shelf.shelf_selected.begin(), shelf.shelf_selected.end(),
                                    [&](const reader::BookDirEntry& entry) { return entry.path == book.path; });
    if (found != shelf.shelf_selected.end()) {
        shelf.shelf_selected.erase(found);
    } else if (idx < static_cast<int>(shelf.shelf_entries.size())) {
        shelf.shelf_selected.push_back(shelf.shelf_entries[idx]);
    } else {
        reader::BookDirEntry entry;
        entry.path = book.path;
        entry.book = book;
        shelf.shelf_selected.push_back(std::move(entry));
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "book selection allocation failed; selection unchanged");
}

void RefreshShelfFooterMode() {
    auto& st = Book_State();
    if (st.shelf.shelf_multi) {
        if (st.shelf.multi_bar != nullptr) {
            lv_obj_set_width(st.shelf.multi_bar, lv_pct(100));
            lv_obj_align(st.shelf.multi_bar, LV_ALIGN_CENTER, 0, -3); // 整体居中并上移 6px
            lv_obj_clear_flag(st.shelf.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.shelf.list_footer != nullptr) {
            lv_obj_set_width(st.shelf.list_footer, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(st.shelf.list_footer, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(st.shelf.list_footer, Book_ItemFont(), 0);
            lv_obj_align(st.shelf.list_footer, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_clear_flag(st.shelf.list_footer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.shelf.list_footer);
        }
        if (st.reader.status_label != nullptr && lv_obj_is_valid(st.reader.status_label)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, ShelfSelectedCount());
            lv_label_set_text(st.reader.status_label, buf);
        }
    } else {
        if (st.shelf.multi_bar != nullptr) {
            lv_obj_add_flag(st.shelf.multi_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(st.shelf.multi_bar, lv_pct(100));
            lv_obj_align(st.shelf.multi_bar, LV_ALIGN_CENTER, 0, 0);
        }
        if (st.shelf.list_footer != nullptr) {
            lv_obj_set_width(st.shelf.list_footer, LV_HOR_RES - 16);
            lv_obj_set_style_text_align(st.shelf.list_footer, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(st.shelf.list_footer, Book_ItemFont(), 0);
            lv_obj_align(st.shelf.list_footer, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(st.shelf.list_footer, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.reader.status_label != nullptr && lv_obj_is_valid(st.reader.status_label)) {
            // 书架页标题在内容区；状态栏仅多选时显示「已选 n」
            lv_label_set_text(st.reader.status_label, "");
        }
    }
}

void ExitShelfMultiMode(bool rebuild) {
    CancelShelfDelete();
    auto& st = Book_State();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.shelf_selected.clear();
    RefreshShelfFooterMode();
    if (rebuild) {
        RequestRenderBookshelfPage();
    }
}

void EnterShelfMultiModeSelect(int idx) {
    auto& st = Book_State();
    st.shelf.shelf_multi = true;
    st.shelf.shelf_selected.clear();
    ToggleShelfItemSelected(idx);
    RefreshShelfFooterMode();
    RequestRenderBookshelfPage();
}

lv_obj_t* FindShelfCell(int index) {
    auto& st = Book_State();
    if (st.shelf.list_body == nullptr || !lv_obj_is_valid(st.shelf.list_body)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(st.shelf.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        auto* item = lv_obj_get_child(st.shelf.list_body, i);
        if (st.shelf.shelf_view == kBookReaderShelfViewList) {
            if (static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item))) == index) {
                return item;
            }
        } else {
            // Grid cells are now inside shelf rows; ignore the decorative base.
            for (uint32_t j = 0; j < lv_obj_get_child_count(item); ++j) {
                auto* cell = lv_obj_get_child(item, j);
                if (lv_obj_has_flag(cell, LV_OBJ_FLAG_CLICKABLE) &&
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))) == index) {
                    return cell;
                }
            }
        }
    }
    return nullptr;
}

lv_obj_t* FindShelfCheckBox(lv_obj_t* cell) {
    if (cell == nullptr) {
        return nullptr;
    }
    // 列表多选：勾选框是行的直接子控件
    const uint32_t n_cell = lv_obj_get_child_count(cell);
    for (uint32_t i = 0; i < n_cell; ++i) {
        lv_obj_t* c = lv_obj_get_child(cell, i);
        if (c != nullptr && (lv_obj_get_width(c) == kShelfCheckSize || lv_obj_get_width(c) == kShelfCoverCheckSize) &&
            lv_obj_get_height(c) == lv_obj_get_width(c)) {
            return c;
        }
    }
    // 封面多选：勾选框挂在封面 host（首子）上
    lv_obj_t* host = lv_obj_get_child(cell, 0);
    if (host == nullptr) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(host, i);
        if (c != nullptr && (lv_obj_get_width(c) == kShelfCheckSize || lv_obj_get_width(c) == kShelfCoverCheckSize) &&
            lv_obj_get_height(c) == lv_obj_get_width(c)) {
            return c;
        }
    }
    return nullptr;
}

void PatchShelfRowCheckMark(int index) {
    auto& st = Book_State();
    if (!st.shelf.shelf_multi) {
        return;
    }
    lv_obj_t* cell = FindShelfCell(index);
    lv_obj_t* check = FindShelfCheckBox(cell);
    if (check == nullptr) {
        RequestRenderBookshelfPage();
        return;
    }
    const bool selected = ShelfItemSelected(index);
    lv_obj_clean(check);
    if (selected) {
        lv_obj_t* mark = lv_label_create(check);
        lv_label_set_text(mark, "√");
        lv_obj_set_style_text_font(mark, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(mark, lv_color_black(), 0);
        lv_obj_center(mark);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    }
    // 列表行：同步选中底色，避免整页重建拖慢跟手震动
    if (st.shelf.shelf_view == kBookReaderShelfViewList && cell != nullptr) {
        if (selected) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xE8E8E8), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }
    }
}

void PatchShelfVisibleCheckMarks() {
    if (ShelfDeleteBusy()) {
        RefreshShelfFooterMode();
        return;
    }
    auto& st = Book_State();
    if (!st.shelf.shelf_multi || st.shelf.list_body == nullptr || !lv_obj_is_valid(st.shelf.list_body)) {
        return;
    }
    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        const uint32_t n = lv_obj_get_child_count(st.shelf.list_body);
        for (uint32_t i = 0; i < n; ++i) {
            auto* cell = lv_obj_get_child(st.shelf.list_body, i);
            PatchShelfRowCheckMark(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))));
        }
    } else {
        const int start = 0;
        const int end = std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.books.size()));
        for (int i = start; i < end; ++i) {
            PatchShelfRowCheckMark(i);
        }
    }
    if (st.reader.status_label != nullptr && lv_obj_is_valid(st.reader.status_label)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, ShelfSelectedCount());
        lv_label_set_text(st.reader.status_label, buf);
    }
}

void OnShelfMultiCancel(lv_event_t* /*e*/) {
    if (ShelfDeleteBusy()) {
        CancelShelfDelete();
        Book_State().shelf.delete_stopping = true;
        RefreshShelfFooterMode();
        return;
    }
    ExitShelfMultiMode(true);
}

void OnShelfMultiSelectAll(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (!st.shelf.shelf_multi || ShelfDeleteBusy()) {
        return;
    }

    if (st.shelf.directory_loading) {
        return;
    }
    const int n = static_cast<int>(st.shelf.books.size());
    const bool clear =
        n > 0 && std::all_of(st.shelf.books.begin(), st.shelf.books.end(), [&](const reader::BookInfo& book) {
            return ShelfItemSelected(static_cast<int>(&book - st.shelf.books.data()));
        });
    for (int i = 0; i < n; ++i) {
        if (ShelfItemSelected(i) == clear) {
            ToggleShelfItemSelected(i);
        }
    }
    RequestShelfCheckMarksPaint();
}

void OnShelfMultiRemove(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (!st.shelf.shelf_multi || st.reader.opening.load()) {
        return;
    }

    StartShelfDelete();
}

void OnBookRowLongPressed(lv_event_t* e) {
    if (ShelfDeleteBusy() || Book_State().shelf.directory_loading) {
        return;
    }
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    if (!st.shelf.shelf_multi) {
        st.shelf.shelf_suppress_click_idx = index;
        st.shelf.shelf_suppress_click_until_us = esp_timer_get_time() + kShelfSuppressRowClickUs;
        EnterShelfMultiModeSelect(index);
        return;
    }
    ToggleShelfItemSelected(index);
    RequestShelfCheckMarksPaint();
}

void OnBookRowClicked(lv_event_t* e) {
    if (Book_State().shelf.directory_loading) {
        return;
    }
    auto& st = Book_State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == st.shelf.shelf_suppress_click_idx && esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(index);
        RequestShelfCheckMarksPaint();
        return;
    }
    lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}
