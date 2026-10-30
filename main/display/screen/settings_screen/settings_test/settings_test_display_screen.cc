#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "settings_test_display_screen.h"

#include "epd_refresh.h"
#include "epdiy.h"
#include "fontpack_lvgl.h"
#include "lv_adapter_epdiy.h"
#include "power_policy.h"
#include "screen_common.h"
#include "screen_text.h"
#include "vk_key_handler.h"

#include <cstring>

namespace {
lv_obj_t* s_screen = nullptr;
lv_obj_t* s_note = nullptr;

lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int width, int height = 64) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    ScreenFitLabel(label, width, height);
    return label;
}

void RunGray() {
    EpdSetTestActive(true);
    // Metalio's native-gray path physically clears before GC16. Never use DU.
    const bool ok = EpdTestRequest(EpdPattern::Gray, MODE_GC16, true);
    if (s_note) lv_label_set_text(s_note, ok
        ? Lang::Strings::SETTINGS_DISPLAY_GRAY_HINT
        : Lang::Strings::SETTINGS_DISPLAY_BUSY);
}

void Button(lv_obj_t* parent, const char* text, int x, int y, int width, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 64);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_update_layout(button);
    ScreenFitLabel(label, lv_obj_get_content_width(button), lv_obj_get_content_height(button));
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

bool Key(const char* key) {
    if (!std::strcmp(key, "vk_prev")) { ScreenNavigateBack(); return true; }
    if (!std::strcmp(key, "vk_home")) { ScreenRequestHome(); return true; }
    if (!std::strcmp(key, "vk_next")) { RunGray(); return true; }
    return false;
}
} // namespace

lv_obj_t* SettingsTestDisplayScreen::Create() {
    auto* screen = lv_obj_create(nullptr);
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ScreenSetIsHome(false);
    ScreenCreateStatusBar(screen);

    Label(screen, Lang::Strings::SETTINGS_TEST_DISPLAY, 24, 76, 620);
    Label(screen, Lang::Strings::SETTINGS_DISPLAY_START_HINT, 24, 130, 620, 110);
    Button(screen, Lang::Strings::SETTINGS_DISPLAY_GRAY, 24, 250, 300, [](lv_event_t*) { RunGray(); });
    Button(screen, Lang::Strings::SETTINGS_DISPLAY_DITHER, 344, 250, 300, [](lv_event_t*) {
        EpdSetTestActive(true);
        const bool ok = EpdTestRequest(EpdPattern::Dither, MODE_GC16, true);
        if (s_note) lv_label_set_text(s_note, ok
            ? Lang::Strings::SETTINGS_DISPLAY_DITHER_HINT
            : Lang::Strings::SETTINGS_DISPLAY_BUSY);
    });
    s_note = Label(screen, Lang::Strings::SETTINGS_DISPLAY_RETEST_HINT, 24, 350, 620, 170);

    // Match Metalio's native Pattern() preview (logical y=540..1089).
    auto* frame = lv_obj_create(screen);
    lv_obj_set_pos(frame, 22, 538);
    lv_obj_set_size(frame, 624, 554);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    Button(screen, Lang::Strings::SETTINGS_TEST_BACK, 24, 1110, 620, [](lv_event_t*) { ScreenNavigateBack(); });

    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        if (lv_event_get_target(e) != s_screen) return;
        s_screen = s_note = nullptr;
        EpdSetTestActive(false);
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
    }, LV_EVENT_DELETE, nullptr);
    VkKey_AttachScreen(screen, "settings_test_display",
                       {.factory = SettingsTestDisplayScreen::Create, .on_key = Key});
    EpdSetTestActive(true);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    if (auto* display = LvAdapterEpdiy::Instance()) display->RequestFullOnNextCommit();
    return screen;
}
