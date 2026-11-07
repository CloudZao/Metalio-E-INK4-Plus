#pragma once

#include "lvgl.h"
#include "reader_types.h"
#include "text_spacing.h"

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

/** @brief 按压缩后的行宽及段首缩进计算整页正文的居中栏宽。 */
inline lv_coord_t PageTextColumnWidth(const Page& page, const lv_font_t* font, lv_coord_t max_width) {
    if (!font || max_width <= 0) return max_width;
    int width = 0;
    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText) return max_width;
        if (item.text.empty()) continue;
        if (item.justify && !item.chapter_title) return max_width;
        const int indent = item.para_indent && !item.chapter_title ? ParaIndentPadPx(font) : 0;
        size_t begin = 0;
        do {
            const size_t end = item.text.find('\n', begin);
            const std::string line = item.text.substr(begin, end == std::string::npos ? end : end - begin);
            int line_width = 0;
            TextLineLetterSpace(font, line, max_width - indent, &line_width);
            width = std::max(width, indent + line_width + (item.chapter_title ? 1 : 0));
            if (end == std::string::npos) break;
            begin = end + 1;
        } while (begin < item.text.size());
    }
    return width > 0 ? std::min<int>(max_width, width) : max_width;
}

/** @brief 标签回退逐字绘制，复用位图路径的像素分配。 */
inline void DrawJustifiedTextLabel(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const std::string text = lv_label_get_text(label);
    lv_area_t area;
    lv_obj_get_content_coords(label, &area);
    const auto spacing = MakeTextLineSpacing(font, text, lv_area_get_width(&area), true);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    lv_obj_init_draw_label_dsc(label, LV_PART_MAIN, &dsc);
    dsc.font = font;
    dsc.color = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    dsc.opa = LV_OPA_COVER;
    int pen = area.x1, index = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(text.data()) + i, text.size() - i, &cp);
        if (!n || cp == '\n' || cp == '\r') break;
        const lv_point_t point = {pen + spacing.Offset(index++), area.y1};
        lv_draw_character(lv_event_get_layer(event), &dsc, &point, cp);
        pen += std::max(1, static_cast<int>(lv_font_get_glyph_width(font, cp, 0)));
        i += n;
    }
}

inline bool IsJustifiedTextLabel(lv_obj_t* label) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(label); ++i) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(label, i)) == DrawJustifiedTextLabel) return true;
    }
    return false;
}

/** @brief 按共享自适应行布局创建正文控件，段距只计入当前项一次。 */
inline lv_obj_t* CreatePageTextLabel(lv_obj_t* col, const PageItem& item, lv_coord_t max_w,
                                     const lv_font_t* book_font, const TextLineLayout& layout,
                                     int& row_index, lv_coord_t para_gap,
                                     void (*decorate_label)(lv_obj_t*) = nullptr) {
    if (!col || !book_font || row_index >= layout.count || item.text.empty()) return nullptr;
    const bool need_para_gap = item.para_gap_before && lv_obj_get_child_count(col) > 0;
    const int gap_before = need_para_gap ? std::max<lv_coord_t>(0, para_gap) : 0;
    const int first = row_index;
    int lines = 1;
    if (item.chapter_title) for (size_t i = 0; i + 1 < item.text.size(); ++i) if (item.text[i] == '\n') ++lines;
    int height = gap_before;
    for (int i = 0; i < lines; ++i) height += layout.CellHeight(row_index++);
    const int offset = layout.TextOffset(first);
    const int underline_extra = std::max<int>(0, layout.line_height - book_font->line_height);
    lv_obj_t* label_parent = col;
    lv_coord_t text_w = max_w;
    if (item.para_indent) {
        const lv_coord_t indent_px = ParaIndentPadPx(book_font);
        auto* row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, max_w, height);
        lv_obj_set_style_pad_top(row, gap_before + offset, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(row);
        auto* spacer = lv_obj_create(row);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_size(spacer, indent_px, 1);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(spacer);
        label_parent = row;
        text_w -= indent_px;
    }
    if (item.chapter_title) {
        auto* stack = lv_obj_create(col);
        lv_obj_remove_style_all(stack);
        lv_obj_set_size(stack, text_w, height);
        lv_obj_clear_flag(stack, LV_OBJ_FLAG_CLICKABLE);
        DisableObjScroll(stack);
        lv_obj_t* first_label = nullptr;
        size_t begin = 0;
        int top = gap_before;
        for (int line = 0; line < lines; ++line) {
            const size_t end = item.text.find('\n', begin);
            const std::string text = item.text.substr(begin, end == std::string::npos ? end : end - begin);
            const int current = first + line;
            for (int dx : {0, 1}) {
                auto* label = lv_label_create(stack);
                lv_label_set_text(label, text.c_str());
                lv_obj_set_style_text_font(label, book_font, 0);
                lv_obj_set_style_text_color(label, lv_color_black(), 0);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_letter_space(label, TextLineLetterSpace(book_font, text, text_w), 0);
                lv_obj_set_style_pad_top(label, layout.TextOffset(current), 0);
                lv_obj_set_style_pad_bottom(label, underline_extra, 0);
                lv_obj_set_size(label, text_w, layout.CellHeight(current));
                lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
                lv_obj_set_pos(label, dx, top);
                lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
                DisableObjScroll(label);
                if (dx == 0 && decorate_label) decorate_label(label);
                if (!first_label) first_label = label;
            }
            top += layout.CellHeight(current);
            begin = end == std::string::npos ? item.text.size() : end + 1;
        }
        return first_label;
    }
    auto* label = lv_label_create(label_parent);
    lv_label_set_text(label, item.text.c_str());
    lv_obj_set_style_text_letter_space(label, TextLineLetterSpace(book_font, item.text, text_w), 0);
    lv_obj_set_style_text_font(label, book_font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(label, underline_extra, 0);
    lv_obj_set_width(label, text_w);
    if (!item.para_indent) {
        lv_obj_set_height(label, height);
        lv_obj_set_style_pad_top(label, gap_before + offset, 0);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    DisableObjScroll(label);
    if (item.justify) {
        lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(label, DrawJustifiedTextLabel, LV_EVENT_DRAW_MAIN_END, nullptr);
    }
    if (decorate_label) decorate_label(label);
    return label;
}

} // namespace reader
