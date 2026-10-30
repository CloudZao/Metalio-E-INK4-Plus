#include "assets/lang_reader.h"
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
#include <cstring>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "reader/book_home_snapshot.h"

// One-bit output needs a stippled texture to retain the reference's light gray blocks.
static const lv_image_dsc_t* EmptyWeekTexture() {
    static const uint8_t pixels[] = {0,255,255,255, 255,255,0,255,
                                   255,255,255,255, 255,255,255,255};
    static const lv_image_dsc_t image = [] {
        lv_image_dsc_t d{};
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_L8;
        d.header.w = 4;
        d.header.h = 4;
        d.header.stride = 4;
        d.data_size = sizeof(pixels);
        d.data = pixels;
        return d;
    }();
    return &image;
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
    const lv_coord_t dash_h = UiSy(164);

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
    const lv_coord_t cont_w = content_w * 49 / 100;
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
        bar_pct = std::clamp(stats.continue_progress_x10 / 10, 0, 100);
        const char* fmt = Lang::Strings::BOOK_READ_PCT_FMT;
        const char* pct = std::strchr(fmt, '%');
        std::snprintf(prog_txt, sizeof(prog_txt), "%.*s %d%%",
                      pct ? static_cast<int>(pct - fmt) : 0, fmt, bar_pct);
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
    lv_obj_set_style_text_font(cont_btn_lbl, list_f, 0);
    lv_obj_set_style_text_color(cont_btn_lbl, lv_color_black(), 0);
    lv_obj_center(cont_btn_lbl);
    lv_obj_clear_flag(cont_btn_lbl, LV_OBJ_FLAG_CLICKABLE);

    const auto week_stats = reader::book_home_snapshot::LoadWeekStats();
    constexpr uint32_t kDaySeconds = 24u * 60u * 60u;
    // 本地日历周，周一到周日；零时长使用淡灰基线。
    lv_obj_t* week = lv_obj_create(dash);
    lv_obj_remove_style_all(week);
    lv_obj_set_size(week, stats_w, dash_h);
    Book_DisableScroll(week);
    lv_obj_clear_flag(week, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* week_title = lv_label_create(week);
    lv_label_set_text(week_title, Lang::Strings::BOOK_WEEK_READING);
    lv_obj_set_style_text_font(week_title, list_f, 0);
    lv_obj_set_pos(week_title, UiSx(12), UiSy(10));
    lv_obj_t* week_value = lv_label_create(week);
    char week_text[48];
    if (week_stats.valid) {
        std::snprintf(week_text, sizeof(week_text), Lang::Strings::BOOK_MINUTES_FMT, static_cast<double>(week_stats.total_seconds) / 60.0);
    } else {
        std::snprintf(week_text, sizeof(week_text), Lang::Strings::BOOK_MINUTES_FMT, static_cast<double>(week_stats.undated_seconds) / 60.0);
    }
    lv_label_set_text(week_value, week_text);
    lv_obj_set_style_text_font(week_value, item_f, 0);
    lv_obj_set_style_text_color(week_value, lv_color_black(), 0);
    lv_obj_set_pos(week_value, UiSx(12), UiSy(46));
    const char* days[] = {Lang::Strings::BOOK_WEEK_MON, Lang::Strings::BOOK_WEEK_TUE, Lang::Strings::BOOK_WEEK_WED, Lang::Strings::BOOK_WEEK_THU, Lang::Strings::BOOK_WEEK_FRI, Lang::Strings::BOOK_WEEK_SAT, Lang::Strings::BOOK_WEEK_SUN};
    const lv_coord_t day_w = stats_w / 7;
    for (int i = 0; i < 7; ++i) {
        lv_obj_t* bar = lv_obj_create(week);
        lv_obj_remove_style_all(bar);
        const lv_coord_t bw = UiSx(16);
        const uint32_t seconds = week_stats.seconds[i];
        // Fixed daily scale: 24 hours fills the chart; a nonzero subpixel value gets one pixel.
        const lv_coord_t bar_h = seconds ? std::max<lv_coord_t>(1,
            static_cast<lv_coord_t>(static_cast<uint64_t>(std::min(seconds, kDaySeconds)) *
                                    UiSy(66) / kDaySeconds)) : UiSy(16);
        lv_obj_set_size(bar, bw, bar_h);
        if (seconds) {
            lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_image_src(bar, EmptyWeekTexture(), 0);
            lv_obj_set_style_bg_image_tiled(bar, true, 0);
        }
        lv_obj_set_pos(bar, i * day_w + (day_w - bw) / 2, dash_h - UiSy(24) - bar_h);
        Book_DisableScroll(bar);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* day = lv_label_create(week);
        lv_label_set_text(day, days[i]);
        lv_obj_set_style_text_font(day, item_f, 0);
        lv_obj_set_width(day, day_w);
        lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(day, i * day_w, dash_h - ih);
    }

    auto make_vgap = [&](lv_coord_t h) {
        lv_obj_t* gap = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(gap);
        lv_obj_set_size(gap, content_w, h);
        lv_obj_clear_flag(gap, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(gap);
    };
    make_vgap(UiSy(34));
    const lv_coord_t stats_block_h = nh + UiSy(8) + ih;
    lv_obj_t* stats_row = lv_obj_create(st.shelf.list_body);
    lv_obj_remove_style_all(stats_row);
    lv_obj_set_size(stats_row, content_w, stats_block_h);
    Book_DisableScroll(stats_row);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_CLICKABLE);
    const lv_coord_t col_w = content_w / 3;
    auto make_stat_col = [&](int index, const char* value, const char* label) {
        lv_obj_t* num = lv_label_create(stats_row);
        lv_label_set_text(num, value);
        lv_obj_set_style_text_font(num, num_font, 0);
        lv_obj_set_width(num, col_w);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(num, index * col_w, 0);
        lv_obj_t* lab = lv_label_create(stats_row);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_font(lab, item_f, 0);
        lv_obj_set_width(lab, col_w);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lab, index * col_w, nh + UiSy(8));
        if (index > 0) {
            lv_obj_t* line = lv_obj_create(stats_row);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, 2, stats_block_h);
            lv_obj_set_style_bg_color(line, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_pos(line, index * col_w - 1, 0);
            Book_DisableScroll(line);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        }
    };
    char duration[24], weekly[24], finished[24];
    FormatHomeDurationShort(duration, sizeof(duration), stats.total_seconds);
    if (week_stats.valid) {
        const bool hours = week_stats.total_seconds >= 3600;
        std::snprintf(weekly, sizeof(weekly), "%.1f%s",
                      static_cast<double>(week_stats.total_seconds) / (hours ? 3600.0 : 60.0),
                      hours ? "h" : "m");
    } else {
        std::snprintf(weekly, sizeof(weekly), "%.1fm", static_cast<double>(week_stats.undated_seconds) / 60.0);
    }
    std::snprintf(finished, sizeof(finished), Lang::Strings::BOOK_COUNT_FMT, stats.finished_count);
    make_stat_col(0, weekly, Lang::Strings::BOOK_WEEK_DURATION);
    make_stat_col(1, duration, Lang::Strings::BOOK_TOTAL_DURATION);
    make_stat_col(2, finished, Lang::Strings::BOOK_FINISHED);
    make_vgap(UiSy(24));

    // 最近阅读：左对齐标题与双封面。
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

        make_vgap(8);

        const lv_coord_t recent_cell_w = (content_w - kHomeGap) / 2;
        lv_obj_update_layout(st.shelf.list_body);
        const lv_coord_t available_h = lv_obj_get_content_height(st.shelf.list_body);
        const lv_coord_t reserved_h = dash_h + UiSy(34) + stats_block_h + UiSy(24) +
            kShelfViewToggleH + 8 + kRecentTitleGap + ih + kHomeShelfTopGap + kHomeShelfBtnH;
        const lv_coord_t recent_cover_h = std::max<lv_coord_t>(UiSy(100),
            std::min<lv_coord_t>(recent_cell_w * 7 / 5, available_h - reserved_h));

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

