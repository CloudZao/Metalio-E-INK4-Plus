#include "assets/lang_screen.h"
#include "settings_test_tab.h"
#include "settings_test_screens.h"
#include "settings_test_display_screen.h"
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
}
