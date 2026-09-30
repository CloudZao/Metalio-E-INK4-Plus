#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"

#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>
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

void SyncShelfSelectedSize() {
    auto& st = Book_State();
    const size_t n = (st.shelf.shelf_view == kBookReaderShelfViewList) ? st.shelf.shelf_entries.size()
                                                                 : st.shelf.books.size();
    if (st.shelf.shelf_selected.size() != n) {
        st.shelf.shelf_selected.assign(n, 0);
    }
}

int ShelfSelectedCount() {
    SyncShelfSelectedSize();
    auto& st = Book_State();
    int n = 0;
    for (uint8_t v : st.shelf.shelf_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool ShelfItemSelected(int idx) {
    SyncShelfSelectedSize();
    auto& st = Book_State();
    return idx >= 0 && idx < static_cast<int>(st.shelf.shelf_selected.size()) &&
           st.shelf.shelf_selected[static_cast<size_t>(idx)] != 0;
}

void ToggleShelfItemSelected(int idx) {
    SyncShelfSelectedSize();
    auto& st = Book_State();
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_selected.size())) {
        return;
    }
    st.shelf.shelf_selected[static_cast<size_t>(idx)] =
        st.shelf.shelf_selected[static_cast<size_t>(idx)] ? 0 : 1;
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
    auto& st = Book_State();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    const size_t n = (st.shelf.shelf_view == kBookReaderShelfViewList) ? st.shelf.shelf_entries.size()
                                                                 : st.shelf.books.size();
    st.shelf.shelf_selected.assign(n, 0);
    RefreshShelfFooterMode();
    if (rebuild) {
        RequestRenderBookshelfPage();
    }
}

void EnterShelfMultiModeSelect(int idx) {
    auto& st = Book_State();
    st.shelf.shelf_multi = true;
    const size_t n = (st.shelf.shelf_view == kBookReaderShelfViewList) ? st.shelf.shelf_entries.size()
                                                                 : st.shelf.books.size();
    st.shelf.shelf_selected.assign(n, 0);
    if (idx >= 0 && idx < static_cast<int>(n)) {
        st.shelf.shelf_selected[static_cast<size_t>(idx)] = 1;
    }
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
        lv_obj_t* cell = lv_obj_get_child(st.shelf.list_body, i);
        if (cell != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))) == index) {
            return cell;
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
        if (c != nullptr && lv_obj_get_width(c) == kShelfCheckSize &&
            lv_obj_get_height(c) == kShelfCheckSize) {
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
        if (c != nullptr && lv_obj_get_width(c) == kShelfCheckSize &&
            lv_obj_get_height(c) == kShelfCheckSize) {
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
    auto& st = Book_State();
    if (!st.shelf.shelf_multi || st.shelf.list_body == nullptr || !lv_obj_is_valid(st.shelf.list_body)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(st.shelf.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(st.shelf.list_body, i);
        if (cell == nullptr) {
            continue;
        }
        PatchShelfRowCheckMark(
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))));
    }
    if (st.reader.status_label != nullptr && lv_obj_is_valid(st.reader.status_label)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, ShelfSelectedCount());
        lv_label_set_text(st.reader.status_label, buf);
    }
}

void OnShelfMultiCancel(lv_event_t* /*e*/) {
    ExitShelfMultiMode(true);
}

void OnShelfMultiSelectAll(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (!st.shelf.shelf_multi) {
        return;
    }
    SyncShelfSelectedSize();
    const int n = static_cast<int>(st.shelf.shelf_selected.size());
    if (n <= 0) {
        return;
    }
    int selected_n = ShelfSelectedCount();
    const bool clear = (selected_n >= n);
    st.shelf.shelf_selected.assign(static_cast<size_t>(n), clear ? 0 : 1);
    RequestShelfCheckMarksPaint();
}

void OnShelfMultiRemove(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (!st.shelf.shelf_multi || st.reader.opening.load()) {
        return;
    }
    SyncShelfSelectedSize();
    if (ShelfSelectedCount() <= 0) {
        return;
    }

    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        // 列表多选：按下标删当前目录项（文件夹整树 / 单书）
        for (int i = static_cast<int>(st.shelf.shelf_entries.size()) - 1; i >= 0; --i) {
            if (i >= static_cast<int>(st.shelf.shelf_selected.size()) ||
                st.shelf.shelf_selected[static_cast<size_t>(i)] == 0) {
                continue;
            }
            const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(i)];
            std::string err;
            if (ent.kind == reader::BookDirEntry::Kind::kFolder) {
                if (!DeleteLibraryFolder(ent.path, err)) {
                    ESP_LOGW(TAG, "batch delete folder fail: %s (%s)", ent.path.c_str(),
                             err.c_str());
                }
            } else {
                if (!DeleteLibraryBookFile(ent.book, err)) {
                    ESP_LOGW(TAG, "batch delete book fail: %s (%s)", ent.path.c_str(), err.c_str());
                    continue;
                }
                const int bi = FindBookIndexByPath(ent.path);
                if (bi >= 0) {
                    st.shelf.books.erase(st.shelf.books.begin() + bi);
                    if (st.shelf.selected == bi) {
                        st.shelf.selected = -1;
                    } else if (st.shelf.selected > bi) {
                        --st.shelf.selected;
                    }
                }
            }
        }
        reader::ReplaceBookLibraryCache(st.shelf.books);
        st.shelf.shelf_multi = false;
        st.shelf.shelf_suppress_click_until_us = 0;
        st.shelf.shelf_suppress_click_idx = -1;
        ReloadShelfDirListing();
        st.shelf.shelf_selected.assign(st.shelf.shelf_entries.size(), 0);
        Book_ClampListPage();
        RefreshShelfFooterMode();
        RequestRenderBookshelfPage();
        return;
    }

    // 封面多选：按书库扁平列表删
    for (int i = static_cast<int>(st.shelf.books.size()) - 1; i >= 0; --i) {
        if (i >= static_cast<int>(st.shelf.shelf_selected.size()) || st.shelf.shelf_selected[static_cast<size_t>(i)] == 0) {
            continue;
        }
        std::string err;
        if (!DeleteLibraryBookFile(st.shelf.books[static_cast<size_t>(i)], err)) {
            ESP_LOGW(TAG, "batch delete fail idx=%d: %s", i, err.c_str());
            continue;
        }
        st.shelf.books.erase(st.shelf.books.begin() + i);
        if (st.shelf.selected == i) {
            st.shelf.selected = -1;
        } else if (st.shelf.selected > i) {
            --st.shelf.selected;
        }
    }
    reader::ReplaceBookLibraryCache(st.shelf.books);
    st.shelf.shelf_multi = false;
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.shelf_selected.assign(st.shelf.books.size(), 0);
    Book_ClampListPage();
    RefreshShelfFooterMode();
    RequestRenderBookshelfPage();
}

void OnBookRowLongPressed(lv_event_t* e) {
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
    auto& st = Book_State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == st.shelf.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(index);
        RequestShelfCheckMarksPaint();
        return;
    }
    lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

