#include "settings_common.h"

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_text.h"

void SettingsStyleBrightnessButton(lv_obj_t* button) {
    lv_obj_set_size(button, kSettingsBrightnessButtonW, kSettingsBrightnessButtonH);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, kSettingsOptionRadius, 0);
    lv_obj_set_style_border_width(button, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    auto* label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
}

void SettingsStyleSelectable(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    lv_obj_set_style_bg_color(btn, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, kSettingsBorderW, 0);
    lv_obj_set_style_radius(btn, kSettingsOptionRadius, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

lv_obj_t* SettingsCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t cb,
                                         intptr_t user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kSettingsOptionH);
    lv_obj_set_style_pad_all(btn, kSettingsOptionPad, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lbl);

    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}
