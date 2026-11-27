#include <algorithm>
#include "ui/common/widgets/screen_options.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_layout.h"
#include "ui/common/drawing/ui_theme.h"

static void FitFixedOption(lv_event_t* event) {
    auto* button = lv_event_get_current_target_obj(event);
    if (lv_obj_get_style_height(button, LV_PART_MAIN) != LV_SIZE_CONTENT) {
        ScreenFitButtonLabelOnSize(event);
    }
}

static void LayoutOptionGrid(lv_event_t* event) {
    auto* grid = lv_event_get_current_target_obj(event);
    const int width = ScreenContentSize(grid).width;
    if (width <= 0) {
        return;
    }
    const auto* font = UiFont();
    const int minimum_width = 3 * ScreenTextHeight(font) + 2 * (kScreenOptionPad + kScreenBorderW);
    const int gap = lv_obj_get_style_pad_column(grid, LV_PART_MAIN);
    const int maximum_columns = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int columns = UiLayoutCount(width, minimum_width, gap, maximum_columns);
    const int button_width = std::max(1, (width - (columns - 1) * gap) / columns);
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        auto* button = lv_obj_get_child(grid, i);
        if (lv_obj_get_style_width(button, LV_PART_MAIN) != button_width) {
            lv_obj_set_width(button, button_width);
        }
    }
}

void ScreenStyleProgressSlider(lv_obj_t* slider) {
    lv_obj_set_height(slider, kScreenProgressSliderH);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UiColorBackground(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, UiColorForeground(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UiColorForeground(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UiColorBackground(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, UiColorForeground(), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, std::max<int>(0, (kScreenProgressKnobSize - kScreenProgressSliderH) / 2), LV_PART_KNOB);
}

lv_obj_t* ScreenCreateOptionGrid(lv_obj_t* parent, int maximum_columns) {
    auto* grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, kScreenOptionGap, 0);
    lv_obj_set_style_pad_row(grid, kScreenOptionGap, 0);
    lv_obj_clear_flag(grid, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* columns = reinterpret_cast<void*>(static_cast<intptr_t>(std::max(1, maximum_columns)));
    lv_obj_add_event_cb(grid, LayoutOptionGrid, LV_EVENT_SIZE_CHANGED, columns);
    lv_obj_add_event_cb(grid, LayoutOptionGrid, LV_EVENT_CHILD_CHANGED, columns);
    return grid;
}

void ScreenStyleSelectable(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    lv_obj_set_style_bg_color(btn, selected ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(btn, kScreenBorderW, 0);
    lv_obj_set_style_radius(btn, kScreenOptionRadius, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, selected ? UiColorBackground() : UiColorForeground(), 0);
        const UiFontRole roles[][2] = {
            {UiFontRole::Body, UiFontRole::Emphasis},
            {UiFontRole::Title, UiFontRole::TitleEmphasis},
            {UiFontRole::Large, UiFontRole::LargeEmphasis},
            {UiFontRole::Caption, UiFontRole::CaptionEmphasis},
        };
        const auto* font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
        for (const auto& pair : roles) {
            if (font == UiFont(pair[0]) || font == UiFont(pair[1])) {
                const auto* selected_font = UiFont(pair[selected ? 1 : 0]);
                if (font != selected_font) {
                    lv_obj_set_style_text_font(lbl, selected_font, 0);
                }
                break;
            }
        }
    }
}

lv_obj_t* ScreenCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t cb, intptr_t user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    const auto* font = UiFont();
    const int minimum = ScreenTextHeight(font) + 2 * (kScreenOptionPad + kScreenBorderW);
    lv_obj_set_style_pad_all(btn, kScreenOptionPad, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    ScreenLayoutTextButton(btn, lbl, std::max<int>(minimum, kScreenOptionH));
    lv_obj_add_event_cb(btn, FitFixedOption, LV_EVENT_SIZE_CHANGED, lbl);

    ScreenStyleSelectable(btn, lbl, false);
    return btn;
}
