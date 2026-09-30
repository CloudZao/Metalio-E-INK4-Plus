#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/book_library.h"

bool ShelfDirIsRoot() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) {
        return true;
    }
    return st.shelf.shelf_dir == reader::kDefaultBooksDir;
}

std::string ShelfParentDir(const std::string& dir) {
    if (dir.empty() || dir == reader::kDefaultBooksDir) {
        return reader::kDefaultBooksDir;
    }
    std::string p = dir;
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return reader::kDefaultBooksDir;
    }
    std::string parent = p.substr(0, slash);
    if (parent.empty() || parent.size() < std::strlen(reader::kDefaultBooksDir)) {
        return reader::kDefaultBooksDir;
    }
    // 不允许越出书库根
    if (parent.rfind(reader::kDefaultBooksDir, 0) != 0) {
        return reader::kDefaultBooksDir;
    }
    return parent;
}

const char* ShelfDirBasename(const std::string& dir) {
    if (dir.empty()) {
        return reader::kDefaultBooksDir;
    }
    const char* base = std::strrchr(dir.c_str(), '/');
    return (base != nullptr && base[1] != '\0') ? (base + 1) : dir.c_str();
}

int FindBookIndexByPath(const std::string& path) {
    auto& st = Book_State();
    for (size_t i = 0; i < st.shelf.books.size(); ++i) {
        if (st.shelf.books[i].path == path) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RefreshShelfViewToggleUi() {
    auto& st = Book_State();
    const bool cover = (st.shelf.shelf_view != kBookReaderShelfViewList);
    auto paint = [](lv_obj_t* btn, lv_obj_t* lbl, bool on) {
        if (btn == nullptr || !lv_obj_is_valid(btn)) {
            return;
        }
        lv_obj_set_style_bg_color(btn, on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (lbl != nullptr && lv_obj_is_valid(lbl)) {
            lv_obj_set_style_text_color(lbl, on ? lv_color_white() : lv_color_black(), 0);
        }
    };
    lv_obj_t* cover_lbl =
        (st.shelf.shelf_view_cover_btn != nullptr) ? lv_obj_get_child(st.shelf.shelf_view_cover_btn, 0) : nullptr;
    lv_obj_t* list_lbl =
        (st.shelf.shelf_view_list_btn != nullptr) ? lv_obj_get_child(st.shelf.shelf_view_list_btn, 0) : nullptr;
    paint(st.shelf.shelf_view_cover_btn, cover_lbl, cover);
    paint(st.shelf.shelf_view_list_btn, list_lbl, !cover);
}

void UpdateShelfTitleLabel() {
    auto& st = Book_State();
    if (st.shelf.shelf_title_lbl == nullptr || !lv_obj_is_valid(st.shelf.shelf_title_lbl)) {
        return;
    }
    const char* raw = Lang::Strings::BOOK_SHELF_TITLE;
    if (st.shelf.shelf_view == kBookReaderShelfViewList && !ShelfDirIsRoot()) {
        raw = ShelfDirBasename(st.shelf.shelf_dir);
    }
    const lv_font_t* font = fontpack_lv_font_get(30, 4);
    if (font == nullptr) {
        font = Book_ListFont();
    }
    // 留给右侧封面|列表切换：两钮 + 竖线 + 间距
    const lv_coord_t toggle_w = kShelfViewBtnW * 2 + 1 + 12;
    const lv_coord_t max_w = Book_ContentWidth() - toggle_w;
    const lv_coord_t budget = max_w > 40 ? max_w : 40;
    lv_obj_set_width(st.shelf.shelf_title_lbl, budget);
    lv_label_set_long_mode(st.shelf.shelf_title_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(st.shelf.shelf_title_lbl, TruncateTextToWidth(raw, font, budget).c_str());
}

void ReloadShelfDirListing() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) {
        st.shelf.shelf_dir = reader::kDefaultBooksDir;
    }
    if (!reader::ScanBookDirListing(st.shelf.shelf_dir.c_str(), st.shelf.shelf_entries)) {
        st.shelf.shelf_entries.clear();
    }
    UpdateShelfTitleLabel();
}

// 扫盘 + 刷列表：须在 LVGL/异步任务跑，禁止 touch_feed 同步调用。
void ShelfReloadAndRenderAsync(void* /*user_data*/) {
    ESP_LOGI(TAG, "async shelf reload dir=%s", Book_State().shelf.shelf_dir.c_str());
    ReloadShelfDirListing();
    RenderBookshelfPage();
}

void ApplyShelfListPageSize() {
    auto& st = Book_State();
    int rows = 1;
    if (st.shelf.shelf_content_h > 0 && kShelfListRowH > 0) {
        rows = static_cast<int>(st.shelf.shelf_content_h / kShelfListRowH);
    }
    if (rows < 1) {
        rows = 1;
    }
    st.shelf.list_page_size = rows;
}

void OnShelfFolderClicked(lv_event_t* e) {
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != reader::BookDirEntry::Kind::kFolder) {
        return;
    }
    st.shelf.shelf_dir = ent.path;
    st.shelf.list_page = 0;
    BookLvAsync(ShelfReloadAndRenderAsync);
}

void OnShelfListEntryLongPressed(lv_event_t* e) {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (!st.shelf.shelf_multi) {
        st.shelf.shelf_suppress_click_idx = idx;
        st.shelf.shelf_suppress_click_until_us = esp_timer_get_time() + kShelfSuppressRowClickUs;
        EnterShelfMultiModeSelect(idx);
        return;
    }
    ToggleShelfItemSelected(idx);
    RequestShelfCheckMarksPaint();
}

void OnShelfListBookClicked(lv_event_t* e) {
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != reader::BookDirEntry::Kind::kBook) {
        return;
    }
    int book_index = FindBookIndexByPath(ent.path);
    if (book_index < 0) {
        st.shelf.books.push_back(ent.book);
        book_index = static_cast<int>(st.shelf.books.size()) - 1;
    }
    lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(book_index)));
}

void SetShelfViewMode(int view) {
    auto& st = Book_State();
    const int next = (view == kBookReaderShelfViewList) ? kBookReaderShelfViewList
                                                        : kBookReaderShelfViewCover;
    if (st.shelf.shelf_view == next) {
        RefreshShelfViewToggleUi();
        return;
    }
    if (st.shelf.shelf_multi) {
        ExitShelfMultiMode(false);
    }
    st.shelf.shelf_view = next;
    BookReaderPrefsSetShelfView(next);
    st.shelf.list_page = 0;
    if (next == kBookReaderShelfViewList) {
        if (st.shelf.shelf_dir.empty()) {
            st.shelf.shelf_dir = reader::kDefaultBooksDir;
        }
        ApplyShelfListPageSize();
        ReloadShelfDirListing();
    } else {
        st.shelf.list_page_size = kShelfCols * kShelfRows;
        UpdateShelfTitleLabel();
    }
    RefreshShelfViewToggleUi();
    RequestRenderBookshelfPage();
}

void OnShelfViewCoverClicked(lv_event_t* /*e*/) {
    SetShelfViewMode(kBookReaderShelfViewCover);
}

void OnShelfViewListClicked(lv_event_t* /*e*/) {
    SetShelfViewMode(kBookReaderShelfViewList);
}

void RenderBookshelfListPage() {
    auto& st = Book_State();
    lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, 0, 0);
    lv_obj_set_style_pad_column(st.shelf.list_body, 0, 0);

    Book_ClampListPage();
    lv_obj_clean(st.shelf.list_body);
    st.shelf.shelf_covers.clear();
    Book_DisableScroll(st.shelf.list_body);
    SyncShelfSelectedSize();

    if (st.shelf.shelf_entries.empty()) {
        Book_ShowMessage(st.shelf.list_body, Lang::Strings::COMMON_EMPTY, Book_ListFont());
        if (st.shelf.list_footer != nullptr) {
            lv_label_set_text(st.shelf.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t row_w = Book_ContentWidth();
    const lv_font_t* item_font = Book_ItemFont();
    const int start = st.shelf.list_page * st.shelf.list_page_size;
    const int end =
        std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.shelf_entries.size()));
    const lv_coord_t lead_w = st.shelf.shelf_multi ? kShelfCheckSize : kShelfListIconW;

    for (int i = start; i < end; ++i) {
        const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(i)];
        const bool is_folder = (ent.kind == reader::BookDirEntry::Kind::kFolder);
        const bool selected = st.shelf.shelf_multi && ShelfItemSelected(i);

        lv_obj_t* row = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, row_w, kShelfListRowH);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (selected) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0xE8E8E8), 0);
        }
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, kShelfListColGap, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(row);
        HapticAttachClick(row);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        if (st.shelf.shelf_multi) {
            // 勾选框在文件名前，避免与进文件夹冲突
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
            if (selected) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, item_font, 0);
                lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                lv_obj_center(mark);
                lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
            }
        } else if (is_folder) {
            // 文件夹：顶标签 + 主体，避免「空框」观感
            lv_obj_t* ico = lv_obj_create(row);
            lv_obj_remove_style_all(ico);
            lv_obj_set_size(ico, kShelfListIconW, 18);
            lv_obj_set_style_bg_opa(ico, LV_OPA_TRANSP, 0);
            Book_DisableScroll(ico);
            lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* tab = lv_obj_create(ico);
            lv_obj_remove_style_all(tab);
            lv_obj_set_size(tab, 9, 5);
            lv_obj_set_style_border_width(tab, 2, 0);
            lv_obj_set_style_border_color(tab, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
            lv_obj_align(tab, LV_ALIGN_TOP_LEFT, 1, 0);
            lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* body = lv_obj_create(ico);
            lv_obj_remove_style_all(body);
            lv_obj_set_size(body, 20, 12);
            lv_obj_set_style_border_width(body, 2, 0);
            lv_obj_set_style_border_color(body, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
            lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        } else {
            // 书籍：折角页 + 两行字线
            lv_obj_t* ico = lv_obj_create(row);
            lv_obj_remove_style_all(ico);
            lv_obj_set_size(ico, kShelfListIconW, 18);
            lv_obj_set_style_border_width(ico, 2, 0);
            lv_obj_set_style_border_color(ico, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(ico, LV_OPA_TRANSP, 0);
            Book_DisableScroll(ico);
            lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* line = lv_obj_create(ico);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, 12, 2);
            lv_obj_set_style_bg_color(line, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_align(line, LV_ALIGN_LEFT_MID, 4, -3);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* line2 = lv_obj_create(ico);
            lv_obj_remove_style_all(line2);
            lv_obj_set_size(line2, 12, 2);
            lv_obj_set_style_bg_color(line2, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(line2, LV_OPA_COVER, 0);
            lv_obj_align(line2, LV_ALIGN_LEFT_MID, 4, 2);
            lv_obj_clear_flag(line2, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* tail = lv_label_create(row);
        lv_obj_set_style_text_font(tail, item_font, 0);
        lv_obj_set_style_text_color(tail, lv_color_black(), 0);
        lv_obj_clear_flag(tail, LV_OBJ_FLAG_CLICKABLE);

        char size_buf[32] = {};
        char tail_buf[48] = {};
        std::string name_src;
        const char* tail_txt = ">";
        if (is_folder) {
            name_src = ent.name;
            name_src.push_back('/');
            lv_label_set_text(tail, ">");
        } else {
            name_src = ent.name;
            reader::FormatFileSize(size_buf, sizeof(size_buf), ent.book.file_size);
            if (BookReaderPrefsShowShelfProgress() != 0) {
                const int pct_x10 = Book_CachedProgressX10OrZero(ent.path.c_str());
                if (pct_x10 >= 1000) {
                    std::snprintf(tail_buf, sizeof(tail_buf), "%s/100%%", size_buf);
                } else {
                    std::snprintf(tail_buf, sizeof(tail_buf), "%s/%d.%d%%", size_buf, pct_x10 / 10,
                                  pct_x10 % 10);
                }
                tail_txt = tail_buf;
                lv_label_set_text(tail, tail_buf);
            } else {
                tail_txt = size_buf;
                lv_label_set_text(tail, size_buf);
            }
            // I1 阈值=16：0x444444 会变白；397 阈值=127 时灰色仍可见
            lv_obj_set_style_text_color(tail, lv_color_black(), 0);
        }
        const lv_coord_t tail_w = Book_MeasureTextWidth(item_font, tail_txt);
        const lv_coord_t name_w =
            row_w - lead_w - kShelfListColGap * 2 - tail_w - kShelfListTailGap;
        const lv_coord_t name_budget = name_w > 24 ? name_w : 24;

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_width(name, name_budget);
        lv_obj_set_style_text_font(name, item_font, 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_label_set_text(name, TruncateFilenameKeepExt(name_src, item_font, name_budget).c_str());
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(name, 1); // lead(0) name(1) tail(2)

        lv_obj_add_event_cb(row, OnShelfListEntryLongPressed, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        if (is_folder) {
            lv_obj_add_event_cb(row, OnShelfFolderClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        } else {
            lv_obj_add_event_cb(row, OnShelfListBookClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        }
    }

    if (st.shelf.list_footer != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", st.shelf.list_page + 1, Book_ListPageCount());
        lv_label_set_text(st.shelf.list_footer, foot);
    }
    RefreshShelfFooterMode();
}

