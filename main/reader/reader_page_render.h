#pragma once

#include "lvgl.h"
#include "reader_types.h"

namespace reader {

inline void DisableObjScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

inline lv_coord_t ParaIndentPadPx(const lv_font_t* font) {
    // 与 TxtParaIndentWidth / 分页 indent_w 一致：优先「一」字宽
    lv_coord_t em = 0;
    if (font != nullptr) {
        em = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, 0x4E00u, 0));
        if (em <= 1) {
            em = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, 0x3000u, 0));
        }
        if (em <= 1) {
            em = font->line_height / 2;
        }
    }
    if (em < 1) {
        em = 12;
    }
    return em * 2;
}

/** 段前垂直空白：独立控件；高度扣除 line_gap，因两侧还有 flex pad_row。 */
inline void AddParaGapSpacer(lv_obj_t* col, lv_coord_t max_w, lv_coord_t para_gap,
                             lv_coord_t line_gap) {
    if (col == nullptr || para_gap <= 0) {
        return;
    }
    lv_coord_t h = para_gap;
    if (h > line_gap) {
        h = static_cast<lv_coord_t>(h - line_gap);
    }
    lv_obj_t* gap = lv_obj_create(col);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, max_w > 0 ? max_w : 1, h);
    lv_obj_clear_flag(gap, LV_OBJ_FLAG_CLICKABLE);
    DisableObjScroll(gap);
}

/**
 * @brief 在 col 下创建正文 label；段首 spacer+label 行布局。
 * @param line_gap / para_gap 须与分页 BookSession::SetGaps 一致
 */
inline lv_obj_t* CreatePageTextLabel(lv_obj_t* col, const PageItem& item, lv_coord_t max_w,
                                     const lv_font_t* book_font, lv_coord_t line_gap,
                                     lv_coord_t para_gap) {
    lv_obj_t* label_parent = col;
    lv_coord_t text_w = max_w;
    const bool need_para_gap =
        item.para_gap_before && col != nullptr && lv_obj_get_child_count(col) > 0;

    if (need_para_gap) {
        AddParaGapSpacer(col, max_w, para_gap, line_gap);
    }

    if (item.para_indent) {
        const lv_coord_t indent_px = ParaIndentPadPx(book_font);
        lv_obj_t* row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, max_w);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(row);

        lv_obj_t* spacer = lv_obj_create(row);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_size(spacer, indent_px, 1);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(spacer);

        label_parent = row;
        text_w = max_w - indent_px;
    }

    // 章名：居中加粗；可含换行拆成「第N章」与章节名两行
    if (item.chapter_title) {
        lv_obj_t* stack = lv_obj_create(label_parent);
        lv_obj_remove_style_all(stack);
        lv_obj_set_width(stack, text_w);
        lv_obj_set_height(stack, LV_SIZE_CONTENT);
        lv_obj_set_style_layout(stack, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(stack, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(stack);

        lv_obj_t* label = lv_label_create(stack);
        lv_label_set_text(label, item.text.c_str());
        lv_obj_set_style_text_font(label, book_font, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, text_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(label);

        lv_obj_t* bold = lv_label_create(stack);
        lv_label_set_text(bold, item.text.c_str());
        lv_obj_set_style_text_font(bold, book_font, 0);
        lv_obj_set_style_text_color(bold, lv_color_black(), 0);
        lv_obj_set_style_text_align(bold, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(bold, text_w);
        lv_label_set_long_mode(bold, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(bold, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(bold);
        lv_obj_align(bold, LV_ALIGN_TOP_LEFT, 1, 0);

        lv_obj_update_layout(stack);
        lv_obj_set_height(stack, lv_obj_get_height(label));
        return label;
    }

    lv_obj_t* label = lv_label_create(label_parent);
    lv_label_set_text(label, item.text.c_str());
    lv_obj_set_style_text_font(label, book_font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_width(label, text_w);
    lv_obj_set_style_flex_grow(label, item.para_indent ? 1 : 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    DisableObjScroll(label);
    return label;
}

}  // namespace reader
