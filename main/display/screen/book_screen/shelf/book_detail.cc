#pragma GCC optimize("O1")

#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/cover/book_cover_loader.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_vk.h"

#include <lvgl.h>
#include <cstdint>
#include <string>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/book_cover_sidecar.h"
#include "reader/reader.h"
#include "screen_common.h"
#include "vk_key_handler.h"

lv_obj_t* CreateDetailScreen(int book_index) {
    auto& st = Book_State();
    if (book_index < 0 || book_index >= static_cast<int>(st.shelf.books.size())) {
        return BookScreen::Create();
    }
    const reader::BookInfo& info = st.shelf.books[static_cast<size_t>(book_index)];
    {
        std::string book_id;
        if (info.format == reader::BookFormat::kEbook) {
            reader::EbookDocument::PeekBookId(info.path.c_str(), book_id);
        }
        ESP_LOGI(TAG, "detail book_id=%s title=%s path=%s",
                 book_id.empty() ? "-" : book_id.c_str(), info.title.c_str(),
                 info.path.c_str());
    }
    st.shelf.selected = book_index;
    CancelDetailCoverLoad();
    // 旧详情若仍在异步删除，封面由 DELETE 回调释放；此处仅在无存活详情时重置
    if (st.detail.detail_scr == nullptr) {
        st.detail.detail_cover.Reset();
    }
    st.detail.detail_format = info.format;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Book_DisableScroll(scr);
    st.detail.detail_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.detail.detail_scr == tgt) {
                s.detail.detail_scr = nullptr;
                s.detail.detail_cover_token.fetch_add(1);
                ClearDetailCoverUiPtrs();
            }
            // 没有更新的详情屏时才释放封面（避免替换瞬间误清）
            if (s.detail.detail_scr == nullptr) {
                s.detail.detail_cover.Reset();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    if (status.status_label) {
        lv_label_set_text(status.status_label, Lang::Strings::BOOK_DETAIL);
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_bottom(body, kDetailCtaBottom, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kDetailSectionGap, 0);
    Book_DisableScroll(body);

    const reader::BookInfo& shown = st.shelf.books[static_cast<size_t>(book_index)];
    const auto peek = reader::BookSession::PeekProgress(shown.path.c_str());
    const lv_font_t* title_font = fontpack_lv_font_get(30, 4); // 书名加粗
    if (title_font == nullptr) {
        title_font = Book_ListFont();
    }
    const lv_font_t* item_font = Book_ItemFont();
    const lv_coord_t title_lh =
        (title_font != nullptr && title_font->line_height > 0) ? title_font->line_height : 30;
    const lv_coord_t item_lh =
        (item_font != nullptr && item_font->line_height > 0) ? item_font->line_height : 25;
    const lv_font_t* num_font = fontpack_lv_font_get(30, 4);
    if (num_font == nullptr) {
        num_font = title_font;
    }

    // 上约 3/5 封面槽略收，下区多给底边呼吸
    const lv_coord_t usable_h =
        (LV_VER_RES - status.height) - 10 - kDetailCtaBottom - kDetailSectionGap;
    const lv_coord_t cover_slot_h = usable_h * 3 / 5 - 12;
    const lv_coord_t bottom_h = usable_h - cover_slot_h;
    {
        lv_coord_t cover_h = cover_slot_h;
        lv_coord_t cover_w = cover_h * 3 / 4;
        const lv_coord_t max_w = Book_ContentWidth();
        if (cover_w > max_w) {
            cover_w = max_w;
            cover_h = cover_w * 4 / 3;
            if (cover_h > cover_slot_h) {
                cover_h = cover_slot_h;
                cover_w = cover_h * 3 / 4;
            }
        }
        if (cover_w < 80) {
            cover_w = 80;
        }
        if (cover_h < 100) {
            cover_h = 100;
        }
        st.detail.detail_cover_fw = cover_w;
        st.detail.detail_cover_fh = cover_h;
    }

    lv_obj_t* cover_host = lv_obj_create(body);
    st.detail.detail_cover_host = cover_host;
    lv_obj_remove_style_all(cover_host);
    lv_obj_set_size(cover_host, Book_ContentWidth(), cover_slot_h);
    lv_obj_set_style_bg_opa(cover_host, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(cover_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cover_host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cover_host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cover_host);

    const int detail_inner_w = CoverFrameInner(st.detail.detail_cover_fw);
    const int detail_inner_h = CoverFrameInner(st.detail.detail_cover_fh);
    const bool sidecar_adequate =
        reader::BookCoverSidecarAdequate(info.path.c_str(), detail_inner_w, detail_inner_h);
    // 有旁路先出图（哪怕偏小）；不够大再后台升权威档，避免书架看过详情仍干等重解
    const bool preview_ok = BookCoverLoader_TryLoadBookSidecarPreview(
        st.shelf.books[static_cast<size_t>(book_index)], detail_inner_w, detail_inner_h, st.detail.detail_cover);
    if (preview_ok && !st.detail.detail_cover.empty()) {
        ApplyDetailCoverImage();
    } else {
        st.detail.detail_cover.Reset();
        FillDetailCoverPlaceholder(cover_host, info.format);
    }
    if (!sidecar_adequate &&
        (info.format == reader::BookFormat::kEpub || info.format == reader::BookFormat::kEbook) &&
        !IsCoverEmbedMiss(info.path)) {
        StartDetailCoverWorker(book_index);
    }

    // 下 2/5：书名 / 作者 / 格式 / 时长 / 继续阅读
    lv_obj_t* bottom = lv_obj_create(body);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_size(bottom, Book_ContentWidth(), bottom_h);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(bottom, 6, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(bottom);

    lv_obj_t* title = lv_label_create(bottom);
    st.detail.detail_title_lbl = title;
    lv_label_set_text(title, shown.title.c_str());
    lv_obj_set_size(title, Book_ContentWidth(), title_lh * kDetailTitleLines);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    Book_DisableScroll(title);

    lv_obj_t* author = lv_label_create(bottom);
    st.detail.detail_meta_lbl = author;
    lv_label_set_text(author, shown.author.empty() ? Lang::Strings::COMMON_UNKNOWN : shown.author.c_str());
    lv_obj_set_size(author, Book_ContentWidth(), item_lh);
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(author, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(author, item_font, 0);
    lv_obj_set_style_text_color(author, lv_color_black(), 0);
    Book_DisableScroll(author);

    {
        lv_obj_t* meta = lv_obj_create(bottom);
        lv_obj_remove_style_all(meta);
        lv_obj_set_size(meta, Book_ContentWidth(), item_lh + 20);
        lv_obj_set_style_layout(meta, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(meta);

        lv_obj_t* fmt = lv_label_create(meta);
        lv_label_set_text(fmt, reader::FormatLabel(shown.format));
        lv_obj_set_style_text_font(fmt, item_font, 0);
        lv_obj_align(fmt, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(fmt, LV_OBJ_FLAG_CLICKABLE);

        char size_buf[16];
        reader::FormatFileSize(size_buf, sizeof(size_buf), shown.file_size);
        lv_obj_t* sz = lv_label_create(meta);
        lv_label_set_text(sz, size_buf);
        lv_obj_set_style_text_font(sz, item_font, 0);
        lv_obj_align(sz, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_clear_flag(sz, LV_OBJ_FLAG_CLICKABLE);

        // 双分界线：上粗(4) 下细(2)
        lv_obj_t* line_bot = lv_obj_create(meta);
        lv_obj_remove_style_all(line_bot);
        lv_obj_set_size(line_bot, Book_ContentWidth(), 2);
        lv_obj_set_style_bg_color(line_bot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line_bot, LV_OPA_COVER, 0);
        lv_obj_align(line_bot, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(line_bot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(line_bot);

        lv_obj_t* line_top = lv_obj_create(meta);
        lv_obj_remove_style_all(line_top);
        lv_obj_set_size(line_top, Book_ContentWidth(), 4);
        lv_obj_set_style_bg_color(line_top, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER, 0);
        lv_obj_align(line_top, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_obj_clear_flag(line_top, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(line_top);
    }

    {
        lv_obj_t* stats = lv_obj_create(bottom);
        lv_obj_remove_style_all(stats);
        lv_obj_set_size(stats, Book_ContentWidth(), item_lh + 4 + 34);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(stats, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(stats);

        auto make_stat = [&](const char* key, uint32_t seconds) {
            lv_obj_t* col = lv_obj_create(stats);
            lv_obj_remove_style_all(col);
            lv_obj_set_width(col, (Book_ContentWidth() - 20) / 2);
            lv_obj_set_height(col, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(col, 4, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(col);
            lv_obj_t* k = lv_label_create(col);
            lv_label_set_text(k, key);
            lv_obj_set_style_text_font(k, item_font, 0);
            lv_obj_set_style_text_color(k, lv_color_black(), 0);
            lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);
            char val[24];
            FormatHomeDurationShort(val, sizeof(val), seconds);
            lv_obj_t* v = lv_label_create(col);
            lv_label_set_text(v, val);
            lv_obj_set_style_text_font(v, num_font, 0);
            lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        };
        make_stat(Lang::Strings::BOOK_TODAY_DURATION, peek.daily_seconds);
        lv_obj_t* sep = lv_obj_create(stats);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, 2, 40);
        lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
        make_stat(Lang::Strings::BOOK_READ_DURATION, peek.reading_seconds);
    }

    {
        const int px10 = peek.progress_x10 >= 0 ? peek.progress_x10 : 0;
        const int bar_pct = px10 / 10;

        lv_obj_t* start_btn = lv_obj_create(bottom);
        lv_obj_remove_style_all(start_btn);
        lv_obj_set_size(start_btn, Book_ContentWidth(), kHomeShelfBtnH);
        lv_obj_set_style_bg_color(start_btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(start_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(start_btn, 14, 0);
        lv_obj_set_style_pad_hor(start_btn, 16, 0);
        lv_obj_set_flex_flow(start_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(start_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(start_btn);
        HapticAttachClick(start_btn);
        lv_obj_add_event_cb(start_btn, OnDetailStartClicked, LV_EVENT_CLICKED, nullptr);

        const lv_coord_t cta_bar_w = Book_ContentWidth() / 2; // 略短，避免压到「继续阅读」
        lv_obj_t* bar_track = lv_obj_create(start_btn);
        lv_obj_remove_style_all(bar_track);
        lv_obj_set_size(bar_track, cta_bar_w, kDetailCtaBarH);
        lv_obj_set_style_bg_color(bar_track, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar_track, kDetailCtaBarH / 2, 0);
        lv_obj_set_style_clip_corner(bar_track, true, 0);
        lv_obj_set_style_pad_all(bar_track, 0, 0);
        lv_obj_set_style_layout(bar_track, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_SCROLLABLE);
        Book_DisableScroll(bar_track);

        if (bar_pct > 0) {
            const lv_coord_t inner_h = kDetailCtaBarH - 2 * kDetailCtaBarInset;
            lv_coord_t fg_w =
                static_cast<lv_coord_t>((cta_bar_w - 2 * kDetailCtaBarInset) * bar_pct / 100);
            if (fg_w < inner_h) {
                fg_w = inner_h;
            }
            if (fg_w > cta_bar_w - 2 * kDetailCtaBarInset) {
                fg_w = cta_bar_w - 2 * kDetailCtaBarInset;
            }
            lv_obj_t* bar_fg = lv_obj_create(bar_track);
            lv_obj_remove_style_all(bar_fg);
            lv_obj_set_size(bar_fg, fg_w, inner_h);
            lv_obj_set_pos(bar_fg, kDetailCtaBarInset, kDetailCtaBarInset);
            lv_obj_set_style_bg_color(bar_fg, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar_fg, inner_h / 2, 0);
            lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(bar_fg);
        }

        lv_obj_t* cta_row = lv_obj_create(start_btn);
        lv_obj_remove_style_all(cta_row);
        lv_obj_set_size(cta_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cta_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cta_row, 8, 0);
        lv_obj_clear_flag(cta_row, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(cta_row);

        lv_obj_t* start_lbl = lv_label_create(cta_row);
        lv_label_set_text(start_lbl,
                          peek.progress_x10 >= 0 ? Lang::Strings::BOOK_CONTINUE
                                                : Lang::Strings::BOOK_START);
        lv_obj_set_style_text_font(start_lbl, Book_ListFont(), 0);
        lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
        lv_obj_clear_flag(start_lbl, LV_OBJ_FLAG_CLICKABLE);

        // 三角边长约等于「读」字高（Book_ListFont ~30）
        const lv_coord_t play_h = title_lh > 0 ? title_lh : 28;
        const lv_coord_t play_w = play_h * 7 / 8;
        lv_obj_t* play = lv_obj_create(cta_row);
        lv_obj_remove_style_all(play);
        lv_obj_set_size(play, play_w, play_h);
        lv_obj_set_style_bg_opa(play, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(play, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(play);
        lv_obj_add_event_cb(
            play,
            [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
                    return;
                }
                lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
                lv_layer_t* layer = lv_event_get_layer(e);
                if (obj == nullptr || layer == nullptr) {
                    return;
                }
                lv_area_t a;
                lv_obj_get_content_coords(obj, &a);
                lv_draw_triangle_dsc_t tri;
                lv_draw_triangle_dsc_init(&tri);
                tri.color = lv_color_white();
                tri.opa = LV_OPA_COVER;
                tri.p[0].x = a.x1;
                tri.p[0].y = a.y1 + 1;
                tri.p[1].x = a.x1;
                tri.p[1].y = a.y2 - 1;
                tri.p[2].x = a.x2;
                tri.p[2].y = (a.y1 + a.y2) / 2;
                lv_draw_triangle(layer, &tri);
            },
            LV_EVENT_DRAW_MAIN, nullptr);
    }

    ScreenSetIsHome(false);
    // factory=ResumeDetail：进百问后返回当前书详情
    VkKey_AttachScreen(scr, kScreenDetail, BookAiLongPressDesc(ResumeDetailScreen));
    return scr;
}

