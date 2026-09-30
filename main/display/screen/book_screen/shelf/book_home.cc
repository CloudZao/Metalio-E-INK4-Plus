#pragma GCC optimize("O1")

#include "book_screen/shelf/book_home.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/cover/book_cover_cell.h"
#include "book_screen/cover/book_cover_style.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/shelf/book_bookshelf.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/book_library.h"

void DrawHomeBarChart(lv_obj_t* parent, lv_coord_t area_w, lv_coord_t area_h, int finished,
                      int book_count) {
    lv_obj_t* chart = lv_obj_create(parent);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, area_w, area_h);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(chart);

    constexpr int kBars = 6;
    const lv_coord_t bar_w = kHomeChartBarW;
    const int seed = std::max(1, book_count) + finished * 3;
    const lv_coord_t heights[kBars] = {
        static_cast<lv_coord_t>(area_h * (35 + (seed * 11) % 45) / 100),
        static_cast<lv_coord_t>(area_h * (50 + (seed * 7) % 40) / 100),
        static_cast<lv_coord_t>(area_h * (70 + (seed * 3) % 25) / 100),
        static_cast<lv_coord_t>(area_h * (45 + (seed * 13) % 40) / 100),
        static_cast<lv_coord_t>(area_h * (55 + (seed * 5) % 35) / 100),
        static_cast<lv_coord_t>(area_h * (40 + (seed * 17) % 40) / 100),
    };
    const lv_coord_t stride = (area_w - bar_w * kBars) / (kBars - 1);
    for (int i = 0; i < kBars; ++i) {
        lv_obj_t* bar = lv_obj_create(chart);
        lv_obj_remove_style_all(bar);
        const lv_coord_t bh = std::max<lv_coord_t>(bar_w + 4, heights[i]);
        lv_obj_set_size(bar, bar_w, bh);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, bar_w / 2, 0); // 顶底圆弧
        lv_obj_set_pos(bar, i * (bar_w + stride), area_h - bh);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(bar);
    }
}

void RenderReadingHome() {
    auto& st = Book_State();
    if (st.shelf.list_body == nullptr) {
        return;
    }
    CancelListCoverFill();
    lv_obj_set_style_layout(st.shelf.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, 0, 0);

    // 先拆控件再释放封面，避免 lv_image 引用失效
    lv_obj_clean(st.shelf.list_body);
    st.shelf.recent_covers.clear();
    Book_DisableScroll(st.shelf.list_body);

    if (st.shelf.list_footer != nullptr) {
        lv_label_set_text(st.shelf.list_footer, "");
        lv_obj_add_flag(st.shelf.list_footer, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.shelf.books.empty()) {
        char tip[160];
        std::snprintf(tip, sizeof(tip), Lang::Strings::BOOK_EMPTY_HOME_FMT,
                      SdUserPath(reader::kDefaultBooksDir));
        Book_ShowMessage(st.shelf.list_body, tip, Book_ListFont());
        return;
    }

    const HomeStats stats = CollectHomeStats(st.shelf.books);
    const lv_coord_t content_w = Book_ContentWidth();

    const lv_font_t* item_f = Book_ItemFont();
    const lv_font_t* list_f = Book_ListFont();
    const lv_font_t* num_font = fontpack_lv_font_get(30, 4);
    if (num_font == nullptr) {
        num_font = list_f;
    }
    const lv_coord_t lh =
        (list_f != nullptr && list_f->line_height > 0) ? list_f->line_height : 30;
    const lv_coord_t ih =
        (item_f != nullptr && item_f->line_height > 0) ? item_f->line_height : 25;
    const lv_coord_t nh =
        (num_font != nullptr && num_font->line_height > 0) ? num_font->line_height : lh;

    // 继续阅读黑卡略抬；余量留给最近阅读封面下单行书名
    const lv_coord_t dash_h =
        kContPad * 2 + lh + kHomeDashInnerGap + std::max(kHomeProgBarH, kHomeThumbW) +
        kHomeDashInnerGap + kContBtnH + kFooterH;

    lv_obj_t* dash = lv_obj_create(st.shelf.list_body);
    lv_obj_remove_style_all(dash);
    lv_obj_set_size(dash, content_w, dash_h);
    lv_obj_set_style_bg_opa(dash, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(dash, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dash, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dash, kHomeGap, 0);
    lv_obj_clear_flag(dash, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(dash);

    // 继续阅读 / 阅读数据 对半
    const lv_coord_t cont_w = (content_w - kHomeGap) / 2;
    const lv_coord_t stats_w = content_w - kHomeGap - cont_w;

    lv_obj_t* cont = lv_obj_create(dash);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, cont_w, dash_h);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, kHomeCardRadius, 0);
    lv_obj_set_style_pad_all(cont, kContPad, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cont);

    char prog_txt[32];
    int bar_pct = 0;
    if (stats.continue_progress_x10 >= 0) {
        FormatBookReadPctLabel(prog_txt, sizeof(prog_txt), stats.continue_progress_x10);
        bar_pct = stats.continue_progress_x10 / 10;
    } else if (stats.continue_index >= 0) {
        std::snprintf(prog_txt, sizeof(prog_txt), "%s", Lang::Strings::BOOK_START);
    } else {
        std::snprintf(prog_txt, sizeof(prog_txt), "%s", Lang::Strings::BOOK_NO_CONTINUE);
    }

    // 百分比 + 进度条成组贴顶，避免 SPACE_BETWEEN 把条甩到中间
    lv_obj_t* top_block = lv_obj_create(cont);
    lv_obj_remove_style_all(top_block);
    lv_obj_set_width(top_block, cont_w - kContPad * 2);
    lv_obj_set_height(top_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(top_block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(top_block, kHomeStatsGap, 0);
    lv_obj_clear_flag(top_block, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(top_block);

    lv_obj_t* cont_sub = lv_label_create(top_block);
    lv_label_set_text(cont_sub, prog_txt);
    lv_obj_set_style_text_font(cont_sub, list_f, 0);
    lv_obj_set_style_text_color(cont_sub, lv_color_white(), 0);
    lv_obj_set_width(cont_sub, cont_w - kContPad * 2);
    lv_obj_clear_flag(cont_sub, LV_OBJ_FLAG_CLICKABLE);

    const lv_coord_t bar_w = cont_w - kContPad * 2;
    lv_obj_t* bar_wrap = lv_obj_create(top_block);
    lv_obj_remove_style_all(bar_wrap);
    lv_obj_set_size(bar_wrap, bar_w, std::max(kHomeProgBarH, kHomeThumbW));
    lv_obj_set_style_bg_opa(bar_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_layout(bar_wrap, LV_LAYOUT_NONE, 0);
    lv_obj_clear_flag(bar_wrap, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(bar_wrap);

    lv_obj_t* bar_track = lv_obj_create(bar_wrap);
    lv_obj_remove_style_all(bar_track);
    lv_obj_set_size(bar_track, bar_w, kHomeProgBarH);
    lv_obj_align(bar_track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_track, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_track, kHomeProgBarH / 2, 0);
    lv_obj_set_style_clip_corner(bar_track, true, 0);
    lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(bar_track);

    // 白底轨道 + 黑色已读；内缩留白缝，避免黑填与卡面黑底糊边
    constexpr lv_coord_t kBarInset = 2;
    if (bar_pct > 0) {
        const lv_coord_t inner_h = kHomeProgBarH - 2 * kBarInset;
        lv_coord_t fg_w =
            static_cast<lv_coord_t>((bar_w - 2 * kBarInset) * bar_pct / 100);
        if (fg_w < inner_h) {
            fg_w = inner_h;
        }
        if (fg_w > bar_w - 2 * kBarInset) {
            fg_w = bar_w - 2 * kBarInset;
        }
        lv_obj_t* bar_fg = lv_obj_create(bar_track);
        lv_obj_remove_style_all(bar_fg);
        lv_obj_set_size(bar_fg, fg_w, inner_h);
        lv_obj_set_pos(bar_fg, kBarInset, kBarInset);
        lv_obj_set_style_bg_color(bar_fg, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar_fg, inner_h / 2, 0);
        lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(bar_fg);
    }

    lv_obj_t* thumb = lv_obj_create(bar_wrap);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, kHomeThumbW, kHomeThumbW);
    lv_obj_set_style_bg_color(thumb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(thumb, 2, 0);
    lv_obj_set_style_border_color(thumb, lv_color_black(), 0);
    lv_coord_t thumb_x = 0;
    if (bar_pct > 0) {
        thumb_x = static_cast<lv_coord_t>((bar_w - kHomeThumbW) * bar_pct / 100);
    }
    lv_obj_set_pos(thumb, thumb_x, (std::max(kHomeProgBarH, kHomeThumbW) - kHomeThumbW) / 2);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(thumb);

    lv_obj_t* cont_btn = lv_obj_create(cont);
    lv_obj_remove_style_all(cont_btn);
    lv_obj_set_size(cont_btn, bar_w, kContBtnH);
    lv_obj_set_style_bg_color(cont_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cont_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont_btn, 8, 0);
    Book_DisableScroll(cont_btn);
    if (stats.continue_index >= 0) {
        lv_obj_add_flag(cont_btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cont_btn);
        lv_obj_add_event_cb(cont_btn, OnContinueReadingClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(stats.continue_index)));
    } else {
        lv_obj_clear_flag(cont_btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* cont_btn_lbl = lv_label_create(cont_btn);
    lv_label_set_text(cont_btn_lbl, Lang::Strings::BOOK_CONTINUE);
    lv_obj_set_style_text_font(cont_btn_lbl, item_f, 0);
    lv_obj_set_style_text_color(cont_btn_lbl, lv_color_black(), 0);
    lv_obj_center(cont_btn_lbl);
    lv_obj_clear_flag(cont_btn_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 右侧统计：柱图高度封顶，下方数字区固定，避免重叠
    lv_obj_t* stats_box = lv_obj_create(dash);
    lv_obj_remove_style_all(stats_box);
    lv_obj_set_size(stats_box, stats_w, dash_h);
    lv_obj_set_style_bg_opa(stats_box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(stats_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stats_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stats_box, 6, 0);
    lv_obj_clear_flag(stats_box, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(stats_box);

    // 数字+下划线+文案（含 pad_row×2），柱图高度据此封顶，避免叠到竖条上
    const lv_coord_t stats_block_h = nh + 4 + 2 + 4 + ih;
    const lv_coord_t chart_h =
        std::max<lv_coord_t>(36, dash_h - stats_block_h - 6);
    DrawHomeBarChart(stats_box, stats_w, chart_h, stats.finished_count,
                     static_cast<int>(st.shelf.books.size()));

    lv_obj_t* stats_row = lv_obj_create(stats_box);
    lv_obj_remove_style_all(stats_row);
    lv_obj_set_size(stats_row, stats_w, stats_block_h);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_clip_corner(stats_row, true, 0);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(stats_row);

    auto make_stat_col = [&](const char* value, const char* label) {
        const lv_coord_t col_w = (stats_w - 8) / 2;
        lv_obj_t* col = lv_obj_create(stats_row);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, col_w, stats_block_h);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(col);

        lv_obj_t* num = lv_label_create(col);
        lv_label_set_text(num, value);
        lv_obj_set_style_text_font(num, num_font, 0);
        lv_obj_set_width(num, col_w);
        lv_label_set_long_mode(num, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(num, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* line = lv_obj_create(col);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, col_w, 2);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(line);

        lv_obj_t* lab = lv_label_create(col);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_font(lab, item_f, 0);
        lv_obj_set_width(lab, col_w);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    };

    char dur_val[24];
    FormatHomeDurationShort(dur_val, sizeof(dur_val), stats.total_seconds);
    char fin_val[24];
    std::snprintf(fin_val, sizeof(fin_val), Lang::Strings::BOOK_COUNT_FMT, stats.finished_count);
    make_stat_col(dur_val, Lang::Strings::BOOK_READ_DURATION);
    make_stat_col(fin_val, Lang::Strings::BOOK_FINISHED);

    // 封面恢复比例高度；余白落在继续阅读与最近阅读之间
    auto make_vgap = [&](lv_coord_t h) {
        lv_obj_t* g = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(g);
        lv_obj_set_size(g, content_w, h);
        lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    };

    lv_obj_t* mid_spacer = lv_obj_create(st.shelf.list_body);
    lv_obj_remove_style_all(mid_spacer);
    lv_obj_set_width(mid_spacer, content_w);
    lv_obj_set_flex_grow(mid_spacer, 1);
    lv_obj_set_style_bg_opa(mid_spacer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mid_spacer, LV_OBJ_FLAG_CLICKABLE);

    // 最近阅读：左标题，右「进度」开关（同书架封面/列表按钮样式）
    {
        lv_obj_t* recent_hdr = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(recent_hdr);
        lv_obj_set_size(recent_hdr, content_w, kShelfViewToggleH);
        lv_obj_set_style_bg_opa(recent_hdr, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(recent_hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(recent_hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(recent_hdr, 8, 0);
        Book_DisableScroll(recent_hdr);
        lv_obj_clear_flag(recent_hdr, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* recent_lbl = lv_label_create(recent_hdr);
        lv_label_set_text(recent_lbl, Lang::Strings::BOOK_RECENT);
        lv_obj_set_style_text_font(recent_lbl, list_f, 0);
        lv_obj_set_style_text_color(recent_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(recent_lbl, LV_OBJ_FLAG_CLICKABLE);

        const bool pct_on = BookReaderPrefsShowShelfProgress() != 0;
        lv_obj_t* pct_btn = lv_obj_create(recent_hdr);
        lv_obj_remove_style_all(pct_btn);
        lv_obj_set_size(pct_btn, kShelfViewBtnW, kShelfViewBtnH);
        lv_obj_set_style_border_width(pct_btn, 2, 0);
        lv_obj_set_style_border_color(pct_btn, lv_color_black(), 0);
        lv_obj_set_style_radius(pct_btn, 4, 0);
        lv_obj_set_style_bg_color(pct_btn, pct_on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(pct_btn, pct_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(pct_btn, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(pct_btn);
        HapticAttachClick(pct_btn);
        lv_obj_add_event_cb(
            pct_btn,
            [](lv_event_t* /*e*/) {
                BookReaderPrefsSetShowShelfProgress(BookReaderPrefsShowShelfProgress() ? 0 : 1);
                RenderReadingHome();
            },
            LV_EVENT_CLICKED, nullptr);
        lv_obj_t* pct_lbl = lv_label_create(pct_btn);
        lv_label_set_text(pct_lbl, "进度");
        lv_obj_set_style_text_font(pct_lbl, item_f, 0);
        lv_obj_set_style_text_color(pct_lbl, pct_on ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(pct_lbl);
        lv_obj_clear_flag(pct_lbl, LV_OBJ_FLAG_CLICKABLE);

        make_vgap(8);

        const lv_coord_t recent_cell_w = (content_w - kHomeGap) / 2;
        lv_coord_t recent_cover_h =
            std::min(kRecentCoverH, static_cast<lv_coord_t>(recent_cell_w * 4 / 3));
        if (recent_cover_h < 140) {
            recent_cover_h = 140;
        }

        lv_obj_t* recent_row = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(recent_row);
        lv_obj_set_width(recent_row, content_w);
        lv_obj_set_height(recent_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(recent_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(recent_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(recent_row, kHomeGap, 0);
        lv_obj_clear_flag(recent_row, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(recent_row);

        auto make_empty_recent_slot = [&](lv_obj_t* parent) {
            lv_obj_t* host = lv_obj_create(parent);
            lv_obj_remove_style_all(host);
            lv_obj_set_size(host, recent_cell_w, recent_cover_h);
            lv_obj_set_style_pad_all(host, kCoverFrameBorder, 0);
            lv_obj_set_style_bg_color(host, lv_color_hex(0xF0F0F0), 0);
            lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
            StyleBookCoverFrame(host);
            lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(host);

            lv_obj_t* empty_lbl = lv_label_create(host);
            lv_label_set_text(empty_lbl, Lang::Strings::BOOK_NO_RECENT);
            lv_obj_set_style_text_font(empty_lbl, list_f, 0);
            lv_obj_set_style_text_color(empty_lbl, lv_color_black(), 0);
            lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(empty_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(empty_lbl, CoverFrameInner(recent_cell_w) - 8);
            lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
            return host;
        };

        for (int k = 0; k < 2; ++k) {
            const int idx = stats.has_recent ? stats.recent_indices[k] : -1;
            if (idx >= 0 && idx < static_cast<int>(st.shelf.books.size())) {
                CreateBookCoverCell(recent_row, st.shelf.books[static_cast<size_t>(idx)], idx,
                                    recent_cell_w, recent_cover_h, st.shelf.recent_covers, false,
                                    kRecentTitleGap, true);
            } else {
                make_empty_recent_slot(recent_row);
            }
        }

        make_vgap(kHomeShelfTopGap);
    }

    // —— 底部「我的书架 >」——
    lv_obj_t* shelf_btn = lv_obj_create(st.shelf.list_body);
    lv_obj_remove_style_all(shelf_btn);
    lv_obj_set_size(shelf_btn, content_w, kHomeShelfBtnH);
    lv_obj_set_style_bg_color(shelf_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shelf_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shelf_btn, kHomeShelfBtnRadius, 0);
    lv_obj_add_flag(shelf_btn, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(shelf_btn);
    HapticAttachClick(shelf_btn);
    lv_obj_add_event_cb(shelf_btn, OnOpenShelfClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* shelf_lbl = lv_label_create(shelf_btn);
    char shelf_txt[64];
    std::snprintf(shelf_txt, sizeof(shelf_txt), "%s  >", Lang::Strings::BOOK_SHELF_TITLE);
    lv_label_set_text(shelf_lbl, shelf_txt);
    lv_obj_set_style_text_font(shelf_lbl, list_f, 0);
    lv_obj_set_style_text_color(shelf_lbl, lv_color_white(), 0);
    lv_obj_center(shelf_lbl);
    lv_obj_clear_flag(shelf_lbl, LV_OBJ_FLAG_CLICKABLE);

    ScheduleListCoverFill();
}

