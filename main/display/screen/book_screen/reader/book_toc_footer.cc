#pragma GCC optimize("O1")

#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/cover/book_cover_loader.h"
#include "book_screen/cover/book_cover_style.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "board.h"
#include "display_orient.h"
#include "haptic_feedback.h"
#include "reader/reader.h"

void ResetPointerLongPress() {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset_long_press(indev);
        }
    }
}

void AppendFooterPart(char* buf, size_t buf_len, const char* part) {
    if (buf == nullptr || buf_len == 0 || part == nullptr || part[0] == '\0') {
        return;
    }
    const size_t used = std::strlen(buf);
    if (used >= buf_len) {
        return;
    }
    if (used == 0) {
        std::snprintf(buf, buf_len, "%s", part);
        return;
    }
    std::snprintf(buf + used, buf_len - used, " · %s", part);
}

void OnFooterClockTick(lv_timer_t* /*t*/) {
    UpdateReadFooter();
}

void SyncFooterClockTimer() {
    auto& st = Book_State();
    BookReaderPrefsEnsureLoaded();
    const int mask = BookReaderPrefsFooterMask();
    const bool need = st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr) && mask != 0 &&
                      ((mask & (kBookReaderFooterTime | kBookReaderFooterBattery)) != 0);
    if (need) {
        if (st.reader.footer_clock_timer == nullptr) {
            st.reader.footer_clock_timer = lv_timer_create(OnFooterClockTick, 30 * 1000, nullptr);
        }
        return;
    }
    if (st.reader.footer_clock_timer != nullptr) {
        lv_timer_del(st.reader.footer_clock_timer);
        st.reader.footer_clock_timer = nullptr;
    }
}

void StopFooterClockTimer() {
    auto& st = Book_State();
    if (st.reader.footer_clock_timer != nullptr) {
        lv_timer_del(st.reader.footer_clock_timer);
        st.reader.footer_clock_timer = nullptr;
    }
}

void UpdateReadFooter() {
    auto& st = Book_State();
    if (st.reader.page_label == nullptr) {
        return;
    }
    char buf[128];
    const lv_font_t* footer_font = Book_ItemFont();
    const lv_coord_t footer_w = LV_HOR_RES - 16;
    if (!st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    const int pct_x10 = st.reader.session->ReadingProgressX10();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        const int n = st.reader.session->TocCount();
        const int pages = std::max(1, (n + st.reader.toc_page_size - 1) / st.reader.toc_page_size);
        std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_TOC_PAGE_FMT, st.reader.toc_list_page + 1, pages);
        lv_label_set_text(st.reader.page_label, buf);
        return;
    }

    const int mask = BookReaderPrefsFooterMask();
    if (mask == 0) {
        lv_label_set_text(st.reader.page_label, "");
        return;
    }

    char hint[16] = "";
    const bool busy = IsLayoutHintBusy();
    const bool show_done = !busy && st.reader.layout_hint_done_until_us > 0 &&
                           esp_timer_get_time() < st.reader.layout_hint_done_until_us;
    if (busy) {
        std::snprintf(hint, sizeof(hint), "%s", Lang::Strings::BOOK_LAYOUT_BUSY);
    } else if (show_done) {
        std::snprintf(hint, sizeof(hint), "%s", Lang::Strings::BOOK_LAYOUT_DONE);
    }

    char prog[40] = "";
    if ((mask & kBookReaderFooterProgress) != 0) {
        const int cur = st.reader.session->BookCurrentPage() + 1;
        const int total = std::max(1, st.reader.session->BookPageCount());
        std::snprintf(prog, sizeof(prog), "%d.%d%%  %d/%d", pct_x10 / 10, pct_x10 % 10, cur, total);
    }

    char batt[12] = "";
    if ((mask & kBookReaderFooterBattery) != 0) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            if (charging) {
                std::snprintf(batt, sizeof(batt), "%d%%+", level);
            } else {
                std::snprintf(batt, sizeof(batt), "%d%%", level);
            }
        }
    }

    char tim[8] = "";
    if ((mask & kBookReaderFooterTime) != 0) {
        const std::time_t now = std::time(nullptr);
        if (struct tm* tm = std::localtime(&now)) {
            std::strftime(tim, sizeof(tim), "%H:%M", tm);
        }
    }

    char suffix[96] = "";
    AppendFooterPart(suffix, sizeof(suffix), tim);
    AppendFooterPart(suffix, sizeof(suffix), prog);
    AppendFooterPart(suffix, sizeof(suffix), batt);
    AppendFooterPart(suffix, sizeof(suffix), hint);

    if ((mask & kBookReaderFooterTitle) != 0) {
        std::string title = st.reader.session->CurrentTocTitle();
        if (title.empty()) {
            title = Lang::Strings::BOOK_BODY;
        }
        const lv_coord_t suffix_w =
            suffix[0] != '\0' ? Book_MeasureTextWidth(footer_font, suffix) + Book_MeasureTextWidth(footer_font, " · ")
                              : 0;
        const lv_coord_t title_budget = std::max<lv_coord_t>(0, footer_w - suffix_w);
        title = TruncateTextToWidth(title, footer_font, title_budget);
        if (suffix[0] != '\0') {
            std::snprintf(buf, sizeof(buf), "%s · %s", title.c_str(), suffix);
        } else {
            std::snprintf(buf, sizeof(buf), "%s", title.c_str());
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%s", suffix);
    }
    lv_label_set_text(st.reader.page_label, buf);
}

void RenderTocList() {
    auto& st = Book_State();
    if (st.reader.content == nullptr || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    CancelListCoverFill();
    st.reader.open_bar = nullptr;
    st.reader.open_pct_lbl = nullptr;
    SetReadStatusVisible(true);
    SetReadStatusCenterTitle(nullptr);
    lv_obj_clean(st.reader.content);
    // 顶栏盖住正文：书头下移，并与顶栏留一缝，避免封面顶死
    const lv_coord_t top_pad =
        (st.reader.status_h > 0 ? st.reader.status_h : kReadPadTop) + kTocBelowStatus;
    ApplyOverlayListContentPads(top_pad);
    const bool land = DisplayUiIsLandscape();
    const lv_coord_t content_w = Book_ContentWidth();
    const lv_coord_t head_w = land ? kTocCoverW : content_w;
    const lv_coord_t list_w =
        land ? std::max<lv_coord_t>(80, content_w - kTocCoverW - kColRuleW - kTocHeadGap * 2)
             : content_w;
    lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_FLEX, 0);
    if (land) {
        lv_obj_set_flex_flow(st.reader.content, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(st.reader.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                             LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(st.reader.content, 0, 0);
        lv_obj_set_style_pad_column(st.reader.content, kTocHeadGap, 0);
    } else {
        lv_obj_set_flex_flow(st.reader.content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(st.reader.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                             LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(st.reader.content, kTocHeadListGap, 0);
        lv_obj_set_style_pad_column(st.reader.content, 0, 0);
    }
    Book_DisableScroll(st.reader.content);
    // 目录页不挂正文触摸（不退出/不翻页）；章行自带点击
    DetachReadBodyInput(st.reader.content);
    lv_obj_clear_flag(st.reader.content, LV_OBJ_FLAG_CLICKABLE);

    const reader::BookInfo& info = st.reader.session->Info();
    const bool toc_has_page = (info.format == reader::BookFormat::kTxt);
    const int toc_n = st.reader.session->TocCount();

    // 书头：封面 + 书名/作者（白底黑字，顶栏仍用现有状态栏）
    lv_obj_t* head = lv_obj_create(st.reader.content);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, head_w);
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    if (land) {
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(head, kTocHeadGap, 0);
    } else {
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, kTocHeadGap, 0);
    }
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(head);

    lv_obj_t* cover_host = lv_obj_create(head);
    lv_obj_remove_style_all(cover_host);
    lv_obj_set_size(cover_host, kTocCoverW, kTocCoverH);
    lv_obj_set_style_bg_color(cover_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cover_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cover_host, kCoverFrameBorder, 0);
    StyleBookCoverFrame(cover_host);
    lv_obj_clear_flag(cover_host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cover_host);

    const int toc_inner_w = CoverFrameInner(kTocCoverW);
    const int toc_inner_h = CoverFrameInner(kTocCoverH);
    // 详情已删时复用 detail_cover；否则旁路秒读，无则占位并后台抽内嵌回写 .a2i1。
    if (st.detail.detail_scr == nullptr || !lv_obj_is_valid(st.detail.detail_scr)) {
        if (st.detail.detail_cover.empty() || st.detail.detail_cover.width + 8 < toc_inner_w ||
            st.detail.detail_cover.height + 8 < toc_inner_h) {
            st.detail.detail_cover.Reset();
            BookCoverLoader_TryLoadBookDetailSidecar(info, toc_inner_w, toc_inner_h, st.detail.detail_cover);
        }
    }
    if (!st.detail.detail_cover.empty()) {
        st.detail.detail_cover.BindDsc();
        lv_obj_t* img = lv_image_create(cover_host);
        lv_image_set_src(img, &st.detail.detail_cover.dsc);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t* ph = lv_label_create(cover_host);
        lv_label_set_text(ph, reader::FormatLabel(info.format));
        lv_obj_set_style_text_font(ph, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(ph, lv_color_black(), 0);
        lv_obj_center(ph);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);
        EnqueueListCoverFill(info, toc_inner_w, toc_inner_h, cover_host, &st.detail.detail_cover);
        ScheduleListCoverFill();
    }

    lv_obj_t* meta = lv_obj_create(head);
    lv_obj_remove_style_all(meta);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(meta);
    const lv_coord_t meta_w = land ? head_w : (content_w - kTocCoverW - kTocHeadGap);
    if (land) {
        lv_obj_set_width(meta, head_w);
        lv_obj_set_height(meta, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(meta, 8, 0);
    } else {
        lv_obj_set_flex_grow(meta, 1);
        lv_obj_set_height(meta, kTocCoverH); // 与封面同高，便于作者贴底
        lv_obj_set_style_layout(meta, LV_LAYOUT_NONE, 0);
    }
    std::string book_title = st.reader.session->Title();
    if (book_title.empty()) {
        book_title = info.title;
    }
    const lv_font_t* title_font = Book_ListFont();
    const lv_coord_t title_lh =
        (title_font != nullptr && title_font->line_height > 0) ? title_font->line_height : 30;
    lv_obj_t* title_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(title_lbl, title_font, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_set_size(title_lbl, meta_w, title_lh * (land ? 2 : 3)); // 横屏叠在封面下，两行
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_lbl, book_title.c_str());
    if (!land) {
        lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 40); // 相对 meta 顶下移
    }
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    std::string author = st.reader.session->Author();
    if (author.empty()) {
        author = info.author;
    }
    if (author.empty()) {
        author = Lang::Strings::COMMON_UNKNOWN;
    }
    const lv_font_t* author_font = Book_ItemFont();
    const lv_coord_t author_lh =
        (author_font != nullptr && author_font->line_height > 0) ? author_font->line_height : 25;
    lv_obj_t* author_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(author_lbl, author_font, 0);
    lv_obj_set_style_text_color(author_lbl, lv_color_black(), 0);
    lv_obj_set_size(author_lbl, meta_w, author_lh);
    lv_label_set_long_mode(author_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(author_lbl, TruncateTextToWidth(author, author_font, meta_w).c_str());
    if (!land) {
        lv_obj_align(author_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -12); // 相对 meta 底上移 12px
    }
    lv_obj_clear_flag(author_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* col_rule = nullptr;
    if (land) {
        col_rule = Book_AddColRule(st.reader.content);
    }

    const lv_coord_t body_h = lv_obj_get_height(st.reader.content);
    const lv_coord_t usable =
        land ? (body_h - top_pad) : (body_h - top_pad - kTocCoverH - kTocHeadListGap);
    st.reader.toc_page_size = std::max(1, static_cast<int>(usable / std::max<lv_coord_t>(1, kTocRowH)));
    // 进入目录时 toc_list_page==-1：按当前章定位（须在算出 page_size 之后）
    if (st.reader.toc_list_page < 0) {
        const int cur0 = st.reader.session->CurrentTocIndex();
        st.reader.toc_list_page =
            (cur0 >= 0 && st.reader.toc_page_size > 0) ? (cur0 / st.reader.toc_page_size) : 0;
    }
    const int page_count =
        toc_n <= 0 ? 1
                   : static_cast<int>((toc_n + st.reader.toc_page_size - 1) / st.reader.toc_page_size);
    if (st.reader.toc_list_page >= page_count) {
        st.reader.toc_list_page = page_count - 1;
    }

    if (toc_n <= 0) {
        lv_obj_t* empty = lv_label_create(st.reader.content);
        lv_obj_set_style_text_font(empty, Book_ListFont(), 0);
        lv_obj_set_style_text_color(empty, lv_color_black(), 0);
        lv_label_set_text(empty, Lang::Strings::BOOK_TOC_EMPTY);
        lv_obj_set_width(empty, list_w);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        Book_FinishColRule(st.reader.content, col_rule);
        UpdateReadFooter();
        ResetPointerLongPress();
        return;
    }

    lv_obj_t* list = lv_obj_create(st.reader.content);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, list_w);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(list);

    const int begin = st.reader.toc_list_page * st.reader.toc_page_size;
    const int end = std::min(toc_n, begin + st.reader.toc_page_size);
    const int cur = st.reader.session->CurrentTocIndex();
    const lv_font_t* row_font = Book_ItemFont();
    for (int i = begin; i < end; ++i) {
        std::string title;
        int page_1based = -1;
        if (const reader::TocEntry* ent = st.reader.session->TocAt(i)) {
            title = ent->title;
            if (toc_has_page) {
                page_1based = ent->first_page + 1;
            }
        }
        if (title.empty() || title == "未知" || title == "未命名") {
            char b[48];
            std::snprintf(b, sizeof(b), Lang::Strings::BOOK_CHAPTER_FMT, i + 1);
            title = b;
        }
        const bool is_cur = (i == cur);
        const bool next_is_cur = (i + 1 == cur);
        const lv_coord_t line_h =
            (row_font != nullptr && row_font->line_height > 0) ? row_font->line_height : 30;
        const lv_coord_t row_h =
            std::max(kTocRowH, static_cast<lv_coord_t>(line_h + kTocRowPad * 2));

        char page_buf[16] = {};
        if (page_1based > 0) {
            std::snprintf(page_buf, sizeof(page_buf), "%d", page_1based);
        }
        const lv_coord_t page_w = Book_MeasureTextWidth(row_font, page_buf);
        const lv_coord_t title_budget =
            std::max<lv_coord_t>(24, list_w - page_w - (page_w > 0 ? 16 : 0));
        const std::string display_title = TruncateTextToWidth(title, row_font, title_budget);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, list_w, row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        // 当前章：上下横线加粗；上一行若紧挨当前章则不画底线，避免叠细线
        if (is_cur) {
            lv_obj_set_style_border_width(row, kTocLineCur, 0);
            lv_obj_set_style_border_side(
                row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
        } else if (next_is_cur) {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_NONE, 0);
        } else {
            lv_obj_set_style_border_width(row, kTocLineThin, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        }
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_pad_ver(row, kTocRowPad, 0);
        lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        Book_DisableScroll(row);
        lv_obj_add_event_cb(row, OnTocRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) { lv_event_stop_bubbling(ev); },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, row_font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_size(lbl, title_budget, line_h);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, display_title.c_str());
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        if (page_buf[0] != '\0') {
            lv_obj_t* page_lbl = lv_label_create(row);
            lv_obj_set_style_text_font(page_lbl, row_font, 0);
            lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);
            lv_label_set_text(page_lbl, page_buf);
            lv_obj_align(page_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_clear_flag(page_lbl, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    Book_FinishColRule(st.reader.content, col_rule);
    UpdateReadFooter();
    ResetPointerLongPress();
    SetReadStatusVisible(true);
}

