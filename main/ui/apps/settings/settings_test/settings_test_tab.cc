#include <cstring>
#include <lvgl.h>
#include <esp_log.h>
#include "ui/apps/settings/settings_test/settings_test_tab.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "ui/apps/settings/settings_test/settings_test_battery_screen.h"
#include "ui/apps/settings/settings_test/settings_test_display_screen.h"
#include "ui/apps/settings/settings_test/settings_test_margin_screen.h"
#include "ui/apps/settings/settings_test/settings_test_screens.h"
#include "ui/apps/settings/settings_test/settings_test_touch_screen.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_control.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/screen_common_nav.h"

static constexpr const char* TAG = "SettingsTestTab";

static void OnAutoTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open auto test screen");
    ScreenNavigateTo(SettingsTestAutoScreen::Create);
}

static void OnTouchTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open touch test screen");
    ScreenNavigateTo(SettingsTestTouchScreen::Create);
}

static void OnBatteryTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open battery test screen");
    ScreenNavigateTo(SettingsTestBatteryScreen::Create);
}

static void OnAgingTestClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open aging test screen");
    ScreenNavigateTo(SettingsTestAgingScreen::Create);
}

void SettingsTestTab_Build(lv_obj_t* page) {
    auto* tests = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "测试模式" : "Test modes");
    auto* group = SettingsCreateListGroup(tests);
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_AUTO, "", OnAutoTestClicked);
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_TOUCH, "", OnTouchTestClicked);
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_BATTERY, "", OnBatteryTestClicked);
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_AGING, "", OnAgingTestClicked);
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_DISPLAY, "",
                          [](lv_event_t*) { ScreenNavigateTo(SettingsTestDisplayScreen::Create); });
    SettingsCreateListRow(group, LangString_SETTINGS_TEST_MARGIN, "",
                          [](lv_event_t*) { ScreenNavigateTo(SettingsTestMarginScreen::Create); });
    auto* operations = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "系统操作" : "System actions");
    auto* actions = SettingsCreateListGroup(operations);
    SettingsCreateListRow(actions, LangString_SETTINGS_TEST_POWER_OFF, "",
                          [](lv_event_t*) { PowerPolicy::GetInstance().RequestPowerOff(); });
    SettingsCreateListRow(actions, LangString_COMMON_RESTART, "", [](lv_event_t*) {
        if (!SystemRequestReboot()) {
            ESP_LOGE(TAG, "test page reboot request failed");
        }
    });
}
