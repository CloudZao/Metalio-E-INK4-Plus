#include "settings_charging_tab.h"
#include "assets/lang_charging.h"
#include "metalio_power.h"
#include "settings_common.h"

namespace {
constexpr int kCurrentsMa[] = {500, 1000, 1500};
constexpr const char* kTitles[] = {"0.5A", "1A", "1.5A"};
lv_obj_t* s_buttons[3] = {};
lv_obj_t* s_hint = nullptr;

void RefreshOptions() {
    const int current = MetalioPower_GetChargeCurrentMa();
    for (int i = 0; i < 3; ++i) {
        SettingsStyleSelectable(s_buttons[i], lv_obj_get_child(s_buttons[i], 0),
                                kCurrentsMa[i] == current);
    }
}
void OnCurrentClicked(lv_event_t* e) {
    const int ma = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const bool ok = MetalioPower_SetChargeCurrentMa(ma);
    lv_label_set_text(s_hint, ok ? Lang::Strings::SETTINGS_CHARGING_HINT
                               : Lang::Strings::SETTINGS_CHARGING_FAILED);
    RefreshOptions();
}
} // namespace

void SettingsChargingTab_Reset() {
    for (auto& button : s_buttons) button = nullptr;
    s_hint = nullptr;
}
void SettingsChargingTab_Build(lv_obj_t* page) {
    auto* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_CHARGING_CURRENT);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    for (int i = 0; i < 3; ++i) {
        s_buttons[i] = SettingsCreateSelectableOption(page, kTitles[i], OnCurrentClicked,
                                                    kCurrentsMa[i]);
    }
    s_hint = lv_label_create(page);
    lv_label_set_text(s_hint, Lang::Strings::SETTINGS_CHARGING_HINT);
    lv_obj_set_style_text_font(s_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_width(s_hint, lv_pct(100));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    RefreshOptions();
}
