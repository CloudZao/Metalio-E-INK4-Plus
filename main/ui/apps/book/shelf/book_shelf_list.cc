#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include "ui/common/widgets/screen_text.h"
#include "assets/lang_screen.h"
#include "ui/common/drawing/ui_theme.h"
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "boards/common/storage/sd_directory_page.h"
#include "business/reader/book_library.h"
#include "business/reader/list_thumbnail_cache.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/cover/book_cover_style.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/common/widgets/screen_common.h"

bool ShelfDirIsRoot() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) {
        return true;
    }
    return st.shelf.shelf_dir == kDefaultBooksDir;
}

std::string ShelfParentDir(const std::string& dir) {
    if (dir.empty() || dir == kDefaultBooksDir) {
        return kDefaultBooksDir;
    }
    std::string p = dir;
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return kDefaultBooksDir;
    }
    std::string parent = p.substr(0, slash);
    if (parent.empty() || parent.size() < std::strlen(kDefaultBooksDir)) {
        return kDefaultBooksDir;
    }
    // 不允许越出书库根
    if (parent.rfind(kDefaultBooksDir, 0) != 0) {
        return kDefaultBooksDir;
    }
    return parent;
}

const char* ShelfDirBasename(const std::string& dir) {
    if (dir.empty()) {
        return kDefaultBooksDir;
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
        lv_obj_set_style_bg_color(btn, on ? UiColorForeground() : UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (lbl != nullptr && lv_obj_is_valid(lbl)) {
            lv_obj_set_style_text_color(lbl, on ? UiColorBackground() : UiColorForeground(), 0);
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
    const std::string title = (ShelfDirIsRoot() || !st.shelf.shelf_query.empty())
                                  ? LangString_BOOK_SHELF_TITLE
                                  : "~" + st.shelf.shelf_dir.substr(std::strlen(kDefaultBooksDir));
    const char* raw = title.c_str();
    // 留给右侧封面|列表切换：两钮 + 竖线 + 间距
    const lv_coord_t toggle_w = kShelfViewBtnW * 2 + 1 + 12;
    const lv_coord_t max_w = Book_ContentWidth() - toggle_w;
    const lv_coord_t budget = max_w > 40 ? max_w : 40;
    lv_obj_set_width(st.shelf.shelf_title_lbl, budget);
    lv_label_set_long_mode(st.shelf.shelf_title_lbl, LV_LABEL_LONG_DOT);
    Book_SetFilenameMiddle(st.shelf.shelf_title_lbl, raw);
}

static SdDirectoryPage s_shelf_directory;
static lv_timer_t* s_shelf_directory_timer = nullptr;
static std::string s_requested_dir;
static int s_requested_page = -1;
static int s_requested_size = 0;
static uint32_t s_directory_revision = 0;

static bool ShelfDirectoryFilter(const char* name, bool directory) {
    return directory || DetectFormatByPath(name) != BookFormat::kUnknown;
}

void CancelShelfDirectory() {
    CancelShelfDelete();
    Book_State().shelf.screen_change_pending.store(false);
    s_shelf_directory.Cancel();
    ClearListThumbnailCache();
    if (s_shelf_directory_timer) {
        lv_timer_del(s_shelf_directory_timer);
    }
    s_shelf_directory_timer = nullptr;
    s_requested_page = -1;
}

int ShelfDirectoryPages() {
    return s_shelf_directory.Pages();
}

static void ShelfDirectoryTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!st.shelf.list_scr || !st.shelf.list_body || st.shelf.back_root != BookUiState::NavRoot::kShelf ||
        SdAccessIsQuiescing()) {
        CancelShelfDirectory();
        return;
    }
    SdDirectoryPage::Result result;
    const bool received = s_shelf_directory.Poll(result);
    if (!received && st.shelf.directory_loading && !st.shelf.directory_loading_shown &&
        esp_timer_get_time() - st.shelf.directory_requested_us >= 180000) {
        st.shelf.directory_loading_shown = true;
        RenderBookshelfPage();
    }
    if (received) {
        if (result.page != st.shelf.list_page) {
            return;
        }
        if (result.ok && result.entries.empty() && result.page > 0) {
            --st.shelf.list_page;
            RenderBookshelfPage();
            return;
        }
        CancelListCoverFill();
        lv_obj_clean(st.shelf.list_body);
        st.shelf.shelf_covers.clear();
        st.shelf.books.clear();
        st.shelf.shelf_entries.clear();
        for (auto& item : result.entries) {
            BookDirEntry entry;
            entry.path = item.path;
            entry.name = item.path.substr(item.path.find_last_of('/') + 1);
            entry.kind = item.directory ? BookDirEntry::Kind::kFolder : BookDirEntry::Kind::kBook;
            entry.book.path = entry.path;
            entry.book.title = item.directory ? entry.name : TitleFromPath(entry.name.c_str());
            entry.book.format = DetectFormatByPath(entry.name.c_str());
            entry.book.file_size = item.bytes;
            entry.book.mtime = item.mtime;
            st.shelf.books.push_back(entry.book);
            st.shelf.shelf_entries.push_back(std::move(entry));
        }
        st.shelf.directory_loading = false;
        st.shelf.directory_failed = !result.ok;
        st.shelf.directory_error = result.error;
        st.shelf.loaded_page = result.page;
        RenderBookshelfPage();
        if (!result.ok) {
            CancelShelfDirectory();
            return;
        }
    }
    if (!st.shelf.directory_loading && st.shelf.list_footer) {
        if (s_shelf_directory.Counted()) {
            lv_label_set_text_fmt(st.shelf.list_footer, "%d/%d", st.shelf.list_page + 1, ShelfDirectoryPages());
            lv_timer_del(s_shelf_directory_timer);
            s_shelf_directory_timer = nullptr;
        } else {
            char text[32];
            std::snprintf(text, sizeof(text), "%d/…", st.shelf.list_page + 1);
            if (std::strcmp(lv_label_get_text(st.shelf.list_footer), text)) {
                lv_label_set_text(st.shelf.list_footer, text);
            }
        }
    }
}

bool EnsureShelfDirectoryPage() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) {
        st.shelf.shelf_dir = kDefaultBooksDir;
    }
    if (s_directory_revision != BookLibraryRevision()) {
        s_directory_revision = BookLibraryRevision();
        s_shelf_directory.Cancel();
        s_requested_page = -1;
        st.shelf.list_page = 0;
    }
    if (s_requested_dir != st.shelf.shelf_dir || s_requested_page != st.shelf.list_page ||
        s_requested_size != st.shelf.list_page_size) {
        if (s_requested_dir != st.shelf.shelf_dir) {
            if (!s_requested_dir.empty()) {
                st.shelf.screen_change_pending.store(true);
            }
            st.shelf.shelf_selected.clear();
            st.shelf.shelf_multi = false;
        }
        s_requested_dir = st.shelf.shelf_dir;
        s_requested_page = st.shelf.list_page;
        s_requested_size = st.shelf.list_page_size;
        st.shelf.directory_loading_shown = false;
        st.shelf.directory_requested_us = esp_timer_get_time();
        st.shelf.directory_loading = true;
        st.shelf.directory_failed = false;
        st.shelf.loaded_page = -1;
        CancelListCoverFill();
        const bool searching = !st.shelf.shelf_query.empty();
        s_shelf_directory.Request({searching ? kDefaultBooksDir : s_requested_dir}, ShelfDirectoryFilter,
                                  s_requested_page, s_requested_size, false, nullptr, true,
                                  st.shelf.shelf_query, searching);
        if (!s_shelf_directory_timer) {
            s_shelf_directory_timer = lv_timer_create(ShelfDirectoryTick, 100, nullptr);
        }
        if (!s_shelf_directory_timer) {
            st.shelf.directory_loading = false;
            st.shelf.directory_failed = true;
            ESP_LOGE(TAG, "directory page timer creation failed");
        }
    }
    const bool ready = !st.shelf.directory_loading && st.shelf.loaded_page == st.shelf.list_page;
    // 等待目录时仍保留旧控件，只有目标页就绪后才能消费翻页重刷请求。
    if (ready && !st.shelf.directory_failed && st.shelf.list_scr == lv_screen_active() &&
        st.shelf.screen_change_pending.exchange(false)) {
        EpdCancelPageTurn();
        EpdRequestScreenChange();
        lv_obj_invalidate(st.shelf.list_scr);
    }
    return ready;
}

void ReloadShelfDirListing() {
    s_requested_page = -1;
    s_shelf_directory.Cancel();
    EnsureShelfDirectoryPage();
    UpdateShelfTitleLabel();
}

// 扫盘 + 刷列表：须在 LVGL/异步任务跑，禁止 touch_feed 同步调用。
void ShelfReloadAndRenderAsync(void* /*user_data*/) {
    ESP_LOGD(TAG, "shelf directory page refresh");
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
    if (Book_State().shelf.directory_loading) {
        return;
    }
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx && esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != BookDirEntry::Kind::kFolder) {
        return;
    }
    st.shelf.shelf_dir = ent.path;
    st.shelf.list_page = 0;
    BookLvAsync(ShelfReloadAndRenderAsync);
}

void OnShelfListEntryLongPressed(lv_event_t* e) {
    if (Book_State().shelf.directory_loading) {
        return;
    }
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
    if (Book_State().shelf.directory_loading) {
        return;
    }
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx && esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != BookDirEntry::Kind::kBook) {
        return;
    }
    int book_index = FindBookIndexByPath(ent.path);
    if (book_index < 0) {
        st.shelf.books.push_back(ent.book);
        book_index = static_cast<int>(st.shelf.books.size()) - 1;
    }
    if (lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(book_index))) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "operation=navigate stage=enqueue result=full");
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
    }
}

void SetShelfViewMode(int view) {
    auto& st = Book_State();
    const int next = (view == kBookReaderShelfViewList) ? kBookReaderShelfViewList : kBookReaderShelfViewCover;
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
    CancelShelfDirectory();
    if (next == kBookReaderShelfViewList) {
        if (st.shelf.shelf_dir.empty()) {
            st.shelf.shelf_dir = kDefaultBooksDir;
        }
        ApplyShelfListPageSize();
        ReloadShelfDirListing();
    } else {
        st.shelf.list_page_size = kShelfCols * kShelfRows;
        UpdateShelfTitleLabel();
    }
    RefreshShelfViewToggleUi();
    RequestRenderBookshelfPage(true);
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
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, 0, 0);
    lv_obj_set_style_pad_column(st.shelf.list_body, 0, 0);

    Book_ClampListPage();
    lv_obj_clean(st.shelf.list_body);
    st.shelf.shelf_covers.clear();
    Book_DisableScroll(st.shelf.list_body);

    if (st.shelf.shelf_entries.empty()) {
        Book_ShowMessage(st.shelf.list_body,
                         st.shelf.shelf_query.empty() ? LangString_BOOK_FOLDER_EMPTY : LangString_BOOK_SEARCH_EMPTY,
                         Book_ListFont());
        if (st.shelf.list_footer != nullptr) {
            lv_label_set_text(st.shelf.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t row_w = Book_ContentWidth();
    const lv_font_t* item_font = Book_ItemFont();
    const int start = 0;
    const int end = std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.shelf_entries.size()));
    const lv_coord_t lead_w = st.shelf.shelf_multi ? kShelfCheckSize : kShelfListIconW;

    for (int i = start; i < end; ++i) {
        const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(i)];
        const bool is_folder = (ent.kind == BookDirEntry::Kind::kFolder);
        const bool selected = st.shelf.shelf_multi && ShelfItemSelected(i);

        lv_obj_t* row = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, row_w, kShelfListRowH);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (selected) {
            lv_obj_set_style_bg_color(row, UiColorSurface(0xE8E8E8), 0);
        }
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, UiColorForeground(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
            lv_obj_set_style_bg_color(check, UiColorBackground(), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(check, UiColorForeground(), 0);
            lv_obj_set_style_border_width(check, kRowBorderW, 0);
            lv_obj_set_style_radius(check, 4, 0);
            Book_DisableScroll(check);
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            if (selected) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, item_font, 0);
                lv_obj_set_style_text_color(mark, UiColorForeground(), 0);
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
            lv_obj_set_style_border_color(tab, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
            lv_obj_align(tab, LV_ALIGN_TOP_LEFT, 1, 0);
            lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* body = lv_obj_create(ico);
            lv_obj_remove_style_all(body);
            lv_obj_set_size(body, 20, 12);
            lv_obj_set_style_border_width(body, 2, 0);
            lv_obj_set_style_border_color(body, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
            lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        } else {
            // 书籍：折角页 + 两行字线
            lv_obj_t* ico = lv_obj_create(row);
            lv_obj_remove_style_all(ico);
            lv_obj_set_size(ico, kShelfListIconW, 18);
            lv_obj_set_style_border_width(ico, 2, 0);
            lv_obj_set_style_border_color(ico, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(ico, LV_OPA_TRANSP, 0);
            Book_DisableScroll(ico);
            lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* line = lv_obj_create(ico);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, 12, 2);
            lv_obj_set_style_bg_color(line, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_align(line, LV_ALIGN_LEFT_MID, 4, -3);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* line2 = lv_obj_create(ico);
            lv_obj_remove_style_all(line2);
            lv_obj_set_size(line2, 12, 2);
            lv_obj_set_style_bg_color(line2, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(line2, LV_OPA_COVER, 0);
            lv_obj_align(line2, LV_ALIGN_LEFT_MID, 4, 2);
            lv_obj_clear_flag(line2, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* tail = lv_label_create(row);
        lv_obj_set_style_text_font(tail, item_font, 0);
        lv_obj_set_style_text_color(tail, UiColorForeground(), 0);
        lv_obj_clear_flag(tail, LV_OBJ_FLAG_CLICKABLE);

        std::string name_src;
        const char* tail_txt = ">";
        if (is_folder) {
            name_src = ent.name;
            name_src.push_back('/');
            lv_label_set_text(tail, ">");
        } else {
            name_src = ent.name;
            tail_txt = "";
            lv_label_set_text(tail, tail_txt);
            EnqueueListCoverFill(ent.book, 0, 0, row, nullptr);
            // I1 阈值=16：0x444444 会变白；397 阈值=127 时灰色仍可见
            lv_obj_set_style_text_color(tail, UiColorForeground(), 0);
        }
        const lv_coord_t tail_w = ScreenMeasureTextWidth(item_font, tail_txt);
        const lv_coord_t name_w = row_w - lead_w - kShelfListColGap * 2 - tail_w - kShelfListTailGap;
        const lv_coord_t name_budget = name_w > 24 ? name_w : 24;

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_width(name, name_budget);
        lv_obj_set_style_text_font(name, item_font, 0);
        lv_obj_set_style_text_color(name, UiColorForeground(), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_label_set_text(name, TruncateFilenameKeepExt(name_src, item_font, name_budget).c_str());
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(name, 1);  // lead(0) name(1) tail(2)

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
    EpdAfterNextSubmit(ScheduleListCoverFill);
}

void ApplyShelfEntryMetadata(const BookInfo& info, lv_obj_t* host, bool progress_ready) {
    auto& st = Book_State();
    if (st.shelf.back_root != BookUiState::NavRoot::kShelf || st.shelf.directory_loading || !st.shelf.list_scr ||
        lv_obj_get_screen(host) != st.shelf.list_scr) {
        return;
    }
    const int index = FindBookIndexByPath(info.path);
    if (index < 0) {
        return;
    }
    const auto entry = std::find_if(
        st.shelf.shelf_entries.begin(), st.shelf.shelf_entries.end(),
        [&](const BookDirEntry& item) { return item.kind == BookDirEntry::Kind::kBook && item.path == info.path; });
    if (entry == st.shelf.shelf_entries.end()) {
        return;
    }
    st.shelf.books[index].file_size = info.file_size;
    st.shelf.books[index].mtime = info.mtime;
    entry->book.file_size = info.file_size;
    entry->book.mtime = info.mtime;
    if (st.shelf.shelf_view == kBookReaderShelfViewCover) {
        if (progress_ready && BookReaderPrefsShowShelfProgress() != 0) {
            auto* badge = Book_CreateProgressPctBadge(host, Book_CachedProgressX10OrZero(info.path.c_str()));
            if (badge) {
                lv_obj_align(badge, LV_ALIGN_TOP_LEFT, kCoverSpineInset + kCoverFrameBorder + 4, 8);
            }
        }
        return;
    }
    auto* name = lv_obj_get_child(host, 1);
    auto* tail = lv_obj_get_child(host, 2);
    if (!name || !tail) {
        return;
    }
    char size_text[32], text[64];
    FormatFileSize(size_text, sizeof(size_text), info.file_size);
    if (progress_ready && BookReaderPrefsShowShelfProgress() != 0) {
        const int pct = Book_CachedProgressX10OrZero(info.path.c_str());
        if (pct >= 1000) {
            std::snprintf(text, sizeof(text), "%s/100%%", size_text);
        } else {
            std::snprintf(text, sizeof(text), "%s/%d.%d%%", size_text, pct / 10, pct % 10);
        }
    } else {
        std::snprintf(text, sizeof(text), "%s", size_text);
    }
    lv_label_set_text(tail, text);
    const lv_coord_t lead = st.shelf.shelf_multi ? kShelfCheckSize : kShelfListIconW;
    const lv_coord_t width = std::max<int>(24, Book_ContentWidth() - lead - kShelfListColGap * 2 -
                                                   ScreenMeasureTextWidth(Book_ItemFont(), text) - kShelfListTailGap);
    lv_obj_set_width(name, width);
    lv_label_set_text(name, TruncateFilenameKeepExt(entry->name, Book_ItemFont(), width).c_str());
}
