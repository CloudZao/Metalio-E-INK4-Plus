#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <lvgl.h>
#include "business/reader/text_encoding.h"
#include "ui/common/drawing/font/ui_fonts.h"

/**
 * @brief 按实际字体度量计算完整文本行高度。
 * @param font 使用的字体，空指针时使用系统字体。
 * @param lines 文本行数，至少为一行。
 * @param line_space 相邻行之间的额外间距。
 * @return 文本高度，不含控件内边距及边框。
 */
static inline int32_t ScreenTextHeight(const lv_font_t* font = nullptr, int32_t lines = 1, int32_t line_space = 0) {
    font = font ? font : UiFont();
    lines = std::max<int32_t>(1, lines);
    return lines * font->line_height + (lines - 1) * line_space;
}

/** @brief 检查字体及其回退链是否包含文本所需字形 */
static inline bool ScreenFontHasText(const lv_font_t* font, const char* text) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t remaining = std::strlen(text);
    while (remaining > 0) {
        uint32_t cp = 0;
        const size_t consumed = Utf8Next(bytes, remaining, &cp);
        if (consumed == 0) {
            return false;
        }
        bytes += consumed;
        remaining -= consumed;
        if (cp == '\n' || cp == '\r') {
            continue;
        }
        lv_font_glyph_dsc_t glyph{};
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || glyph.is_placeholder) {
            return false;
        }
    }
    return true;
}

/** @brief 按可用宽高优先选择完整换行字号；最小字号仍放不下时省略显示。 */
static inline void ScreenFitLabel(lv_obj_t* label, int32_t width, int32_t height,
                                  const lv_font_t* preferred = nullptr) {
    width = std::max<int32_t>(1, width);
    height = std::max<int32_t>(1, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    const lv_font_t* font = preferred ? preferred : UiFont();
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    for (int candidate = 0; candidate <= 1; ++candidate) {
        if (candidate > 0) {
            const auto* smaller = UiFontCompact(font);
            if (smaller == nullptr || smaller->line_height >= font->line_height ||
                !ScreenFontHasText(smaller, lv_label_get_text(label))) {
                continue;
            }
            font = smaller;
        }
        lv_point_t extent;
        lv_text_get_size(&extent, lv_label_get_text(label), font, letter_space, line_space, width, LV_TEXT_FLAG_NONE);
        if (extent.x <= width && extent.y <= height) {
            break;
        }
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, width);
    lv_point_t extent;
    lv_text_get_size(&extent, lv_label_get_text(label), font, letter_space, line_space, width, LV_TEXT_FLAG_NONE);
    const bool fits = extent.x <= width && extent.y <= height;
    lv_obj_set_height(label, fits ? LV_SIZE_CONTENT : height);
    lv_label_set_long_mode(label, fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

/**
 * @brief 让纯文字按钮按换行内容增高，并保留原设计的最小高度。
 * @param button 只含文字标签的按钮。
 * @param label 按钮的文字标签。
 * @param minimum_height 原设计最小高度，包含内边距及边框。
 */
static inline void ScreenLayoutTextButton(lv_obj_t* button, lv_obj_t* label, int32_t minimum_height) {
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(button, minimum_height, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

/** @brief 在按钮尺寸确定后适配其文案 */
static inline void ScreenFitButtonLabelOnSize(lv_event_t* event) {
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    ScreenFitLabel(label, lv_obj_get_content_width(button), lv_obj_get_content_height(button));
    lv_obj_center(label);
}

/** @brief 按实际字体测量文本宽度，空文本返回0。 */
lv_coord_t ScreenMeasureTextWidth(const lv_font_t* font, const char* text);

/** @brief 按像素宽度截断单行文本，保留完整UTF-8字符并追加省略号。 */
std::string ScreenEllipsizeText(const std::string& text, const lv_font_t* font, lv_coord_t max_w);

/** @brief 按像素填充最多两行标题，末行超出时追加省略号。 */
std::string ScreenLayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w);
