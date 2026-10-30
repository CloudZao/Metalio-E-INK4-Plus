#pragma once

#include "fontpack_lvgl.h"

#include <algorithm>
#include <lvgl.h>

/** @brief 按可用宽高选择字库字号并完整换行显示文案 */
static inline void ScreenFitLabel(lv_obj_t* label, int32_t width, int32_t height,
                                  const lv_font_t* preferred = nullptr) {
    width = std::max<int32_t>(1, width);
    const lv_font_t* font = preferred ? preferred : fontpack_lv_font_ui();
    if (font == nullptr) font = LV_FONT_DEFAULT;
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    for (const int size : {0, 25, 18}) {
        if (size != 0) {
            const auto* smaller = fontpack_lv_font_get(size, 2);
            if (smaller == nullptr || smaller->line_height >= font->line_height) continue;
            font = smaller;
        }
        lv_point_t extent;
        lv_text_get_size(&extent, lv_label_get_text(label), font, letter_space, line_space,
                         width, LV_TEXT_FLAG_NONE);
        if (extent.x <= width && extent.y <= height) break;
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, width);
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
