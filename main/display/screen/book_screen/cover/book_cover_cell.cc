#include "book_screen/cover/book_cover_cell.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/cover/book_cover_loader.h"
#include "book_screen/cover/book_cover_style.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_prefs.h"

#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "haptic_feedback.h"
#include "reader/reader.h"

lv_obj_t* CreateCoverSlot(lv_obj_t* parent, const reader::BookInfo& info, lv_coord_t w, lv_coord_t h,
                          reader::RasterImage* cover_out, bool* need_embed_fill) {
    if (need_embed_fill != nullptr) {
        *need_embed_fill = false;
    }
    lv_obj_t* host = lv_obj_create(parent);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, w, h);
    lv_obj_set_style_pad_all(host, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(host, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
    StyleBookCoverFrame(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(host);

    const int inner_w = CoverFrameInner(w);
    const int inner_h = CoverFrameInner(h);
    if (cover_out != nullptr &&
        BookCoverLoader_TryLoadBookDetailSidecar(info, inner_w, inner_h, *cover_out) &&
        !cover_out->empty()) {
        cover_out->BindDsc();
        lv_obj_t* img = lv_image_create(host);
        lv_image_set_src(img, &cover_out->dsc);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        if (cover_out != nullptr) {
            cover_out->Reset();
        }
        lv_obj_t* ph_lbl = lv_label_create(host);
        lv_label_set_text(ph_lbl, reader::FormatLabel(info.format));
        lv_obj_set_style_text_font(ph_lbl, Book_ListFont(), 0);
        lv_obj_set_style_text_color(ph_lbl, lv_color_black(), 0);
        lv_obj_center(ph_lbl);
        lv_obj_clear_flag(ph_lbl, LV_OBJ_FLAG_CLICKABLE);
        if (need_embed_fill != nullptr &&
            (info.format == reader::BookFormat::kEpub ||
             info.format == reader::BookFormat::kEbook)) {
            *need_embed_fill = true;
        }
    }
    return host;
}

// 书架 / 最近阅读：封面 + 可选书名
lv_obj_t* CreateBookCoverCell(lv_obj_t* parent, const reader::BookInfo& info, int index,
                              lv_coord_t cell_w, lv_coord_t cover_h,
                              std::vector<std::unique_ptr<reader::RasterImage>>& cover_store,
                              bool long_press_delete, lv_coord_t title_gap, bool show_title) {
    auto& st = Book_State();
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, cell_w);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, show_title ? title_gap : 0, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cell);
    lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(cell);
    lv_obj_add_event_cb(cell, OnBookRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    if (long_press_delete) {
        lv_obj_add_event_cb(cell, OnBookRowLongPressed, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    }

    auto cover = std::make_unique<reader::RasterImage>();
    bool need_embed_fill = false;
    lv_obj_t* host =
        CreateCoverSlot(cell, info, cell_w, cover_h, cover.get(), &need_embed_fill);
    cover_store.push_back(std::move(cover));
    if (need_embed_fill) {
        EnqueueListCoverFill(info, CoverFrameInner(cell_w), CoverFrameInner(cover_h), host,
                             cover_store.back().get());
    }

    const int pct_x10 = BookReaderPrefsShowShelfProgress() != 0
                            ? Book_CachedProgressX10OrZero(info.path.c_str())
                            : -1;
    if (pct_x10 >= 0) {
        lv_obj_t* badge = Book_CreateProgressPctBadge(host, pct_x10);
        if (badge != nullptr) {
            // 相对书脊第二条竖线内侧再留空（外框 + 书脊 inset + 线宽 + 间隙）
            const lv_coord_t left =
                kCoverSpineInset + kCoverFrameBorder + 4;
            lv_obj_align(badge, LV_ALIGN_TOP_LEFT, left, 8);
        }
    }

    if (long_press_delete && st.shelf.shelf_multi) {
        lv_obj_t* check = lv_obj_create(host);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kShelfCheckSize, kShelfCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, kRowBorderW, 0);
        lv_obj_set_style_radius(check, 4, 0);
        lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -4, 4);
        Book_DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        if (ShelfItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(mark, lv_color_black(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    if (show_title) {
        const lv_font_t* item_font = Book_ItemFont();
        lv_obj_t* title = lv_label_create(cell);
        lv_obj_set_width(title, cell_w);
        lv_obj_set_style_text_font(title, item_font, 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const std::string clipped = TruncateTextToWidth(info.title, item_font, cell_w);
        lv_label_set_text(title, clipped.c_str());
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }
    return cell;
}

