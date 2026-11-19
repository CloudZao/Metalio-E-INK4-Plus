#include "assets/lang_screen.h"
#include "application.h"
#include "power_policy.h"
#include "settings_test_tab.h"
#include "settings_test_screens.h"
#include "settings_test_display_screen.h"
#include "settings_test_margin_screen.h"
#include "settings_test_battery_screen.h"
#include "settings_test_touch_screen.h"
#include "settings_common.h"
#include "assets/lang_config.h"
#include "screen_common.h"
#include <esp_log.h>

namespace {
constexpr const char* TAG = "SettingsTestTab";

void OnAutoTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open auto test screen");
    ScreenNavigateTo(SettingsTestAutoScreen::Create);
}

void OnTouchTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open touch test screen");
    ScreenNavigateTo(SettingsTestTouchScreen::Create);
}

void OnBatteryTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open battery test screen");
    ScreenNavigateTo(SettingsTestBatteryScreen::Create);
}

void OnAgingTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open aging test screen");
    ScreenNavigateTo(SettingsTestAgingScreen::Create);
}

}  // namespace

void SettingsTestTab_Build(lv_obj_t* page) {
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_AUTO, OnAutoTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_TOUCH, OnTouchTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_BATTERY, OnBatteryTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_AGING, OnAgingTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_DISPLAY, [](lv_event_t*) { ScreenNavigateTo(SettingsTestDisplayScreen::Create); }, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_MARGIN,
        [](lv_event_t*) { ScreenNavigateTo(SettingsTestMarginScreen::Create); }, 0);
    auto* power_row = lv_obj_create(page);
    lv_obj_remove_style_all(power_row);
    lv_obj_set_size(power_row, lv_pct(100), kSettingsOptionH);
    lv_obj_set_flex_flow(power_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(power_row, kSettingsTabGap, 0);
    lv_obj_clear_flag(power_row, LV_OBJ_FLAG_SCROLLABLE);
    auto* off = SettingsCreateSelectableOption(power_row, Lang::Strings::SETTINGS_TEST_POWER_OFF,
        [](lv_event_t*) { PowerPolicy::GetInstance().RequestPowerOff(); }, 0);
    auto* restart = SettingsCreateSelectableOption(power_row, Lang::Strings::COMMON_RESTART,
        [](lv_event_t*) { Application::GetInstance().Reboot(); }, 0);
    for (auto* button : {off, restart}) {
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
    }
}
