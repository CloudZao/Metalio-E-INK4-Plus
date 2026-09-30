#include "settings_network_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "network_screen/network_screen.h"
#include "screen_common.h"
#include "settings_common.h"

#include <esp_log.h>

#define TAG "SettingsNetwork"

namespace {

struct NetworkUi {
    lv_obj_t* wifi_cfg_btn = nullptr; // 配置 WIFI 按钮
};

NetworkUi s_ui;

void OnEnterWifiConfigClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "open NetworkScreen for WiFi setup");
    ScreenNavigateTo(NetworkScreen::Create);
}

} // namespace

void SettingsNetworkTab_Reset() {
    s_ui = {};
}

const char* SettingsNetworkTab_CurrentName() {
    return "WiFi";
}

bool SettingsNetworkTab_IsWifi() {
    return true;
}

void SettingsNetworkTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_NET_WIFI_CFG_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_NET_WIFI_CFG_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cur = lv_label_create(page);
    lv_label_set_text(cur, Lang::Strings::SETTINGS_NET_CUR_WIFI);
    lv_obj_set_style_text_font(cur, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(cur, lv_color_black(), 0);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE);

    s_ui.wifi_cfg_btn = SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_NET_ENTER_CFG,
                                                       OnEnterWifiConfigClicked, 0);
    SettingsStyleSelectable(s_ui.wifi_cfg_btn, lv_obj_get_child(s_ui.wifi_cfg_btn, 0), false);
}
