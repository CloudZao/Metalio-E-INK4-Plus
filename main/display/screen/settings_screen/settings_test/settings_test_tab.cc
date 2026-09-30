#include "settings_test_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "screen_common.h"
#include "settings_common.h"
#include "settings_test_battery_screen.h"
#include "settings_test_screens.h"
#include "settings_test_touch_screen.h"

#include <esp_log.h>

#define TAG "SettingsTestTab"

static void OnAutoTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open auto test");
    ScreenNavigateTo(SettingsTestAutoScreen::Create);
}
static void OnTouchTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open touch test");
    ScreenNavigateTo(SettingsTestTouchScreen::Create);
}
static void OnBatteryTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open battery test");
    ScreenNavigateTo(SettingsTestBatteryScreen::Create);
}
static void OnAgingTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open aging test");
    ScreenNavigateTo(SettingsTestAgingScreen::Create);
}

void SettingsTestTab_Reset() {}
void SettingsTestTab_OnActivated() {}
void SettingsTestTab_OnDeactivated() {}

void SettingsTestTab_Build(lv_obj_t* page) {
    // 本轮无 WiFi/4G：信号条仅占位
    lv_obj_t* signal = lv_label_create(page);
    lv_label_set_text(signal, Lang::Strings::SETTINGS_TEST_SIGNAL_SKIP);
    lv_obj_set_style_text_font(signal, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(signal, lv_color_black(), 0);
    lv_obj_clear_flag(signal, LV_OBJ_FLAG_CLICKABLE);

    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_AUTO, OnAutoTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_TOUCH, OnTouchTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_BATTERY, OnBatteryTestClicked, 0);
    SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_TEST_AGING, OnAgingTestClicked, 0);
}
