#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <esp_timer.h>
#include <lvgl.h>
#include "ui/common/widgets/screen_text.h"
#include "assets/lang_reader.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "business/reader/book_cover_sidecar.h"
#include "ui/apps/book/cover/book_cover_style.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/common/drawing/ui_theme.h"

static void SetInformationRow(lv_obj_t* left, lv_obj_t* right, const char* left_text, const char* right_text) {
    auto* parent = lv_obj_get_parent(left);
    lv_obj_update_layout(parent);
    const int width = std::max<int>(0, lv_obj_get_width(parent) - UiSx(16));
    const int right_width = std::min<int>(width / 2, ScreenMeasureTextWidth(Book_ItemFont(), right_text));
    const bool header = parent == Book_State().reader.info_header;
    lv_obj_set_width(right, right_width);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, header ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_TOP_RIGHT, -UiSx(8), 0);
    lv_obj_set_width(left, std::max<int>(0, width - right_width - (right_width ? UiSx(8) : 0)));
    lv_obj_set_style_text_align(left, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(left, header ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_LEFT, UiSx(8), 0);
    if (std::strcmp(lv_label_get_text(left), left_text) != 0) {
        lv_label_set_text(left, left_text);
    }
    if (std::strcmp(lv_label_get_text(right), right_text) != 0) {
        lv_label_set_text(right, right_text);
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

void ResetPointerLongPress() {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset_long_press(indev);
        }
    }
}

void SyncFooterClockTimer() {
    auto& st = Book_State();
    BookReaderPrefsEnsureLoaded();
    const int mask = BookReaderPrefsFooterMask();
    const bool need = st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr) && mask != 0 &&
                      ((mask & (kBookReaderFooterTime | kBookReaderFooterBattery)) != 0);
    if (need) {
        const uint32_t period_ms = (mask & kBookReaderFooterBattery) != 0 ? 1000 : 30000;
        if (st.reader.footer_clock_timer == nullptr) {
            st.reader.footer_clock_timer = lv_timer_create(OnFooterClockTick, period_ms, nullptr);
        } else {
            lv_timer_set_period(st.reader.footer_clock_timer, period_ms);
        }
        return;
    }
    if (st.reader.footer_clock_timer != nullptr) {
        lv_timer_del(st.reader.footer_clock_timer);
        st.reader.footer_clock_timer = nullptr;
    }
}

void StopFooterClockTimer() {
    StopReaderAutoTurn();
    auto& st = Book_State();
    if (st.reader.footer_clock_timer != nullptr) {
        lv_timer_del(st.reader.footer_clock_timer);
        st.reader.footer_clock_timer = nullptr;
    }
}

void UpdateReadFooter() {
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        return;
    }
    if (st.reader.page_label == nullptr) {
        return;
    }
    if (st.reader.footer_host) {
        const bool show_background =
            st.reader.background_image && st.reader.read_chrome != BookUiState::ReadChrome::kToc;
        lv_obj_set_style_bg_opa(st.reader.footer_host, show_background ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        if (st.reader.info_header) {
            lv_obj_set_style_bg_opa(st.reader.info_header, show_background ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        }
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
        std::snprintf(buf, sizeof(buf), LangString_BOOK_TOC_PAGE_FMT, st.reader.toc_list_page + 1, pages);
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
    const bool show_done =
        !busy && st.reader.layout_hint_done_until_us > 0 && esp_timer_get_time() < st.reader.layout_hint_done_until_us;
    if (show_done) {
        std::snprintf(hint, sizeof(hint), "%s", LangString_BOOK_LAYOUT_DONE);
    }

    char percent[16] = "";
    char pages[32] = "";
    char prog[64] = "";
    if ((mask & kBookReaderFooterProgress) != 0) {
        const int cur = st.reader.session->CurrentPage() + 1;
        const int total = std::max(1, st.reader.session->PageCount());
        if (st.reader.session->StructurePending() && st.reader.session->Info().format != BookFormat::kTxt) {
            std::snprintf(percent, sizeof(percent), "--%%");
        } else {
            std::snprintf(percent, sizeof(percent), "%d.%d%%", pct_x10 / 10, pct_x10 % 10);
        }
        if ((st.reader.session->Info().format == BookFormat::kTxt && st.reader.session->TocCount() == 0) ||
            st.reader.session->PageCountsEstimated()) {
            std::snprintf(pages, sizeof(pages), "--/--");
        } else {
            std::snprintf(pages, sizeof(pages), "%d/%d", cur, total);
        }
        std::snprintf(prog, sizeof(prog), "%s  %s", percent, pages);
    }

    char batt[12] = "";
    if ((mask & kBookReaderFooterBattery) != 0) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        const bool battery_ready = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        if (battery_ready) {
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

    if (BookReaderPrefsFooterLayout() == kBookReaderFooterSplit) {
        std::string title;
        if ((mask & kBookReaderFooterTitle) != 0) {
            title = st.reader.session->CurrentTocTitle();
            if (title.empty()) {
                title = LangString_BOOK_BODY;
            }
        }
        char lower_left[32] = "";
        AppendFooterPart(lower_left, sizeof(lower_left), tim);
        AppendFooterPart(lower_left, sizeof(lower_left), batt);
        SetInformationRow(st.reader.info_title, st.reader.info_progress, title.c_str(), percent);
        SetInformationRow(st.reader.page_label, st.reader.info_pages, lower_left, pages);
        return;
    }

    char suffix[96] = "";
    AppendFooterPart(suffix, sizeof(suffix), tim);
    AppendFooterPart(suffix, sizeof(suffix), prog);
    AppendFooterPart(suffix, sizeof(suffix), batt);
    AppendFooterPart(suffix, sizeof(suffix), hint);

    if ((mask & kBookReaderFooterTitle) != 0) {
        std::string title = st.reader.session->CurrentTocTitle();
        if (title.empty()) {
            title = LangString_BOOK_BODY;
        }
        const lv_coord_t suffix_w =
            suffix[0] != '\0' ? ScreenMeasureTextWidth(footer_font, suffix) + ScreenMeasureTextWidth(footer_font, " · ")
                              : 0;
        const lv_coord_t title_budget = std::max<lv_coord_t>(0, footer_w - suffix_w);
        title = ScreenEllipsizeText(title, footer_font, title_budget);
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
    if (st.reader.standby_paused) {
        return;
    }
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
    const lv_coord_t top_pad = (st.reader.status_h > 0 ? st.reader.status_h : kReadPadTop) + kTocBelowStatus;
    ApplyOverlayListContentPads(top_pad);
    const bool land = DisplayUiIsLandscape();
    const lv_coord_t content_w = Book_ContentWidth();
    const lv_coord_t head_w = land ? kTocCoverW : content_w;
    const lv_coord_t list_w =
        land ? std::max<lv_coord_t>(80, content_w - kTocCoverW - kColRuleW - kTocHeadGap * 2) : content_w;
    lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_FLEX, 0);
    if (land) {
        lv_obj_set_flex_flow(st.reader.content, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(st.reader.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(st.reader.content, 0, 0);
        lv_obj_set_style_pad_column(st.reader.content, kTocHeadGap, 0);
    } else {
        lv_obj_set_flex_flow(st.reader.content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(st.reader.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(st.reader.content, kTocHeadListGap, 0);
        lv_obj_set_style_pad_column(st.reader.content, 0, 0);
    }
    Book_DisableScroll(st.reader.content);
    // 目录页不挂正文触摸（不退出/不翻页）；章行自带点击
    DetachReadBodyInput(st.reader.content);
    lv_obj_clear_flag(st.reader.content, LV_OBJ_FLAG_CLICKABLE);

    const BookInfo& info = st.reader.session->Info();
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
    lv_obj_set_style_bg_color(cover_host, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(cover_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cover_host, kCoverFrameBorder, 0);
    StyleBookCoverFrame(cover_host);
    lv_obj_clear_flag(cover_host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cover_host);

    const int toc_inner_w = CoverFrameInner(kTocCoverW);
    const int toc_inner_h = CoverFrameInner(kTocCoverH);
    // 详情已删时复用 detail_cover；否则旁路秒读，无则占位并后台抽内嵌回写 .a2l8。
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
        EpdSetContentRole(img, EpdContentRole::Cover);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t* ph = lv_label_create(cover_host);
        lv_label_set_text(ph, FormatLabel(info.format));
        lv_obj_set_style_text_font(ph, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(ph, UiColorForeground(), 0);
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
        lv_obj_set_height(meta, kTocCoverH);  // 与封面同高，便于作者贴底
        lv_obj_set_style_layout(meta, LV_LAYOUT_NONE, 0);
    }
    std::string book_title = st.reader.session->Title();
    if (book_title.empty()) {
        book_title = info.title;
    }
    const lv_font_t* title_font = Book_ListFont();
    const lv_coord_t title_lh = (title_font != nullptr && title_font->line_height > 0) ? title_font->line_height : 30;
    const lv_font_t* author_font = Book_ItemFont();
    const lv_coord_t author_lh =
        (author_font != nullptr && author_font->line_height > 0) ? author_font->line_height : 25;
    const lv_coord_t title_top = land ? 0 : 40;
    const lv_coord_t author_bottom = 12;
    const lv_coord_t meta_gap = 8;
    // 竖屏为作者预留独立区域，书名仅使用可容纳完整行的高度。
    const lv_coord_t title_lines =
        land
            ? 2
            : std::min<lv_coord_t>(
                  3, std::max<lv_coord_t>(0, kTocCoverH - title_top - author_bottom - author_lh - meta_gap) / title_lh);
    lv_obj_t* title_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(title_lbl, title_font, 0);
    lv_obj_set_style_text_color(title_lbl, UiColorForeground(), 0);
    lv_obj_set_style_text_line_space(title_lbl, 0, 0);
    lv_obj_set_size(title_lbl, meta_w, title_lh * title_lines);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_lbl, book_title.c_str());
    if (!land) {
        lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, title_top);
    }
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    std::string author = st.reader.session->Author();
    if (author.empty()) {
        author = info.author;
    }
    if (author.empty()) {
        author = LangString_COMMON_UNKNOWN;
    }
    lv_obj_t* author_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(author_lbl, author_font, 0);
    lv_obj_set_style_text_color(author_lbl, UiColorForeground(), 0);
    lv_obj_set_size(author_lbl, meta_w, author_lh);
    lv_label_set_long_mode(author_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(author_lbl, ScreenEllipsizeText(author, author_font, meta_w).c_str());
    if (!land) {
        lv_obj_align(author_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -author_bottom);
    }
    lv_obj_clear_flag(author_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* col_rule = nullptr;
    if (land) {
        col_rule = Book_AddColRule(st.reader.content);
    }

    const lv_coord_t body_h = lv_obj_get_height(st.reader.content);
    const lv_coord_t usable = land ? (body_h - top_pad) : (body_h - top_pad - kTocCoverH - kTocHeadListGap);
    st.reader.toc_page_size = std::max(1, static_cast<int>(usable / std::max<lv_coord_t>(1, kTocRowH)));
    // 进入目录时 toc_list_page==-1：按当前章定位（须在算出 page_size 之后）
    if (st.reader.toc_list_page < 0) {
        const int cur0 = st.reader.session->CurrentTocIndex();
        st.reader.toc_list_page = (cur0 >= 0 && st.reader.toc_page_size > 0) ? (cur0 / st.reader.toc_page_size) : 0;
    }
    const int page_count =
        toc_n <= 0 ? 1 : static_cast<int>((toc_n + st.reader.toc_page_size - 1) / st.reader.toc_page_size);
    if (st.reader.toc_list_page >= page_count) {
        st.reader.toc_list_page = page_count - 1;
    }

    if (toc_n <= 0) {
        lv_obj_t* empty = lv_obj_create(st.reader.content);
        lv_obj_remove_style_all(empty);
        lv_obj_set_size(empty, list_w, std::max<lv_coord_t>(1, usable));
        Book_DisableScroll(empty);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        Book_ShowMessage(
            empty, st.reader.session->LiveRelayoutPending() ? LangString_BOOK_TOC_BUILDING : LangString_BOOK_TOC_EMPTY);
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
        lv_coord_t indent = 0;
        if (const TocEntry* ent = st.reader.session->TocAt(i)) {
            title = ent->title;
            indent = std::min<uint16_t>(ent->depth, 3) * 12;
        }
        if (title.empty() ||
            (st.reader.session->Info().format != BookFormat::kEpub && (title == "未知" || title == "未命名"))) {
            char b[48];
            std::snprintf(
                b, sizeof(b),
                st.reader.session->IsPageImagesMode() ? LangString_BOOK_PAGE_FMT : LangString_BOOK_CHAPTER_FMT, i + 1);
            title = b;
        }
        const bool is_cur = (i == cur);
        const bool next_is_cur = (i + 1 == cur);
        const lv_coord_t line_h = (row_font != nullptr && row_font->line_height > 0) ? row_font->line_height : 30;
        const lv_coord_t row_h = std::max(kTocRowH, static_cast<lv_coord_t>(line_h + kTocRowPad * 2));

        const lv_coord_t title_budget = std::max<lv_coord_t>(24, list_w - indent);
        const std::string display_title = ScreenEllipsizeText(title, row_font, title_budget);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, list_w, row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        // 当前章：上下横线加粗；上一行若紧挨当前章则不画底线，避免叠细线
        if (is_cur) {
            lv_obj_set_style_border_width(row, kTocLineCur, 0);
            lv_obj_set_style_border_side(row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                                         0);
        } else if (next_is_cur) {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_NONE, 0);
        } else {
            lv_obj_set_style_border_width(row, kTocLineThin, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        }
        lv_obj_set_style_border_color(row, UiColorForeground(), 0);
        lv_obj_set_style_pad_ver(row, kTocRowPad, 0);
        lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        Book_DisableScroll(row);
        lv_obj_add_event_cb(row, OnTocRowClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, row_font, 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_set_size(lbl, title_budget, line_h);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, display_title.c_str());
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, indent, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);


    }
    Book_FinishColRule(st.reader.content, col_rule);
    UpdateReadFooter();
    ResetPointerLongPress();
    SetReadStatusVisible(true);
}
