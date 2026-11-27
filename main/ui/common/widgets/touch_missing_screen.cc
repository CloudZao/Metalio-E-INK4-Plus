#include <lvgl.h>
#include "ui/common/widgets/touch_missing_screen.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

lv_obj_t* TouchMissingScreen::Create() {
    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* label = lv_label_create(scr);
    lv_label_set_text(label, LangString_TOUCH_MISSING_HINT);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    VkKey_AttachScreen(scr, "touch_missing");
    return scr;
}
