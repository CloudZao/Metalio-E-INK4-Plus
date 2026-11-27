#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <esp_timer.h>
#include <wifi_station.h>
#include "sdkconfig.h"
#include "ui/apps/settings/settings_network_tab.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "assets/lang_screen.h"
#include "boards/common/board/dual_network_board.h"
#include "business/network/network_settings.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_network_signal.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

static constexpr int kSimSlotExternal = 0;
static constexpr int kSimSlotInternal = 1;

struct SettingsNetworkTabNetworkUi {
    lv_obj_t* root_scr = nullptr;
    lv_obj_t* wifi_btn = nullptr;
    lv_obj_t* cell_btn = nullptr;
    lv_obj_t* wifi_cfg_box = nullptr;
    lv_obj_t* wifi_power_btn = nullptr;  // 与快捷面板共用的 Wi-Fi 开关
    int wifi_power_state = -1;           // 已绘制的任务状态、网络模式与开关状态组合
    lv_obj_t* sim_box = nullptr;
    lv_obj_t* sim_ext_btn = nullptr;
    lv_obj_t* sim_int_btn = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* overlay_msg = nullptr;
};

static SettingsNetworkTabNetworkUi s_ui;
static UiPageLifecycle s_page;
static NetworkSettingsRequest s_request;
static NetworkSettingsProgress s_shown_progress = NetworkSettingsProgress::Queued;
static int s_shown_countdown = -1;
static uint32_t s_wifi_update_ms = 0;

static UiModal s_window_modal;

static bool IsCellNetwork() {
    return NetworkSettingsCurrentType() == NetworkType::CELLULAR;
}

static bool NetworkSettingsBusy() {
    return s_request && (!s_request->completed.load(std::memory_order_acquire) ||
                         (s_request->operation == NetworkSettingsOperation::SwitchNetwork && s_request->ok));
}

static const char* SimSlotName(int slot) {
    return (slot == kSimSlotInternal) ? LangString_SETTINGS_NET_SIM_INT : LangString_SETTINGS_NET_SIM_EXT;
}

static void CloseOverlay() {
    if (s_ui.overlay != nullptr) {
        lv_obj_delete(s_ui.overlay);
    }
    s_ui.overlay = nullptr;
    s_ui.overlay_msg = nullptr;
}

static void OpenOverlay(const char* msg) {
    if (s_ui.root_scr == nullptr) {
        return;
    }
    CloseOverlay();

    lv_obj_t* mask = lv_obj_create(s_ui.root_scr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;
    s_window_modal.Bind(mask, [](void*) { CloseOverlay(); });

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(80), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, UiSy(96), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, UiSx(20), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.overlay_msg = lbl;
    lv_label_set_text(lbl, msg != nullptr ? msg : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
}

static void SetOverlayText(const char* msg) {
    if (s_ui.overlay_msg != nullptr) {
        lv_label_set_text(s_ui.overlay_msg, msg != nullptr ? msg : "");
    }
}

static void RefreshSimUi() {
    if (s_ui.sim_ext_btn == nullptr || s_ui.sim_int_btn == nullptr) {
        return;
    }
    const int slot = NetworkSettingsSimSlot();
    const bool ext = (slot == kSimSlotExternal);
    SettingsSetListRowCurrent(s_ui.sim_ext_btn, ext);
    SettingsSetListRowCurrent(s_ui.sim_int_btn, !ext);
}

static void SetSimSectionVisible(bool visible) {
    if (s_ui.sim_box == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_ui.sim_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.sim_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void SetWifiConfigSectionVisible(bool visible) {
    if (s_ui.wifi_cfg_box == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void RefreshWifiPower(lv_timer_t* /*timer*/) {
    if (!s_ui.wifi_power_btn) {
        return;
    }
    const auto phase = NetworkWifiGetSwitchState();
    const bool available = !IsCellNetwork();
    const bool enabled = available && WifiStation::GetInstance().IsUserEnabled();
    const int state = static_cast<int>(phase) * 4 + available * 2 + enabled;
    if (state == s_ui.wifi_power_state) {
        return;
    }
    s_ui.wifi_power_state = state;
    const bool switching = phase == NetworkWifiSwitchState::kSwitching || !available;
    lv_obj_set_state(s_ui.wifi_power_btn, LV_STATE_CHECKED, enabled);
    lv_obj_set_state(s_ui.wifi_power_btn, LV_STATE_DISABLED, switching);
}

static void OnWifiPowerClicked(lv_event_t* /*event*/) {
    if (IsCellNetwork()) {
        return;
    }
    NetworkWifiToggle();
    RefreshWifiPower(nullptr);
}

static void RefreshNetworkOptions() {
    const bool busy = NetworkSettingsBusy();
    const bool switching = busy && s_request->operation == NetworkSettingsOperation::SwitchNetwork;
    const bool is_cell = switching ? s_request->target_type == NetworkType::CELLULAR : IsCellNetwork();
    if (s_ui.wifi_btn != nullptr) {
        SettingsSetListRowCurrent(s_ui.wifi_btn, !is_cell);
    }
    if (s_ui.cell_btn != nullptr) {
        SettingsSetListRowCurrent(s_ui.cell_btn, is_cell);
    }
    // Wi-Fi 模式显示无线开关，4G 模式显示 SIM 卡选择。
    SetWifiConfigSectionVisible(!is_cell);
    SetSimSectionVisible(is_cell);
    if (is_cell) {
        RefreshSimUi();
    }
    for (auto* button : {s_ui.wifi_btn, s_ui.cell_btn, s_ui.sim_ext_btn, s_ui.sim_int_btn}) {
        if (button) {
            lv_obj_set_state(button, LV_STATE_DISABLED, busy);
        }
    }
}

[[maybe_unused]] static void OnNetworkOptionClicked(lv_event_t* e) {
    if (NetworkSettingsBusy()) {
        return;
    }
    const auto target = static_cast<NetworkType>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (NetworkSettingsCurrentType() == target) {
        RefreshNetworkOptions();
        return;
    }
    s_request = NetworkSettingsSwitchType(target);
    RefreshNetworkOptions();
    if (!s_request) {
        ScreenShowToast(LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}

static void ScheduleSimSlotQuery() {
    s_request = NetworkSettingsCurrent();
    if (!s_request) {
        s_request = NetworkSettingsQuerySim();
    }
    if (!s_request) {
        ScreenShowToast(LangString_SETTINGS_NET_SIM_TASK_FAIL);
    }
}

static void ScheduleSimSwitch(int target_slot) {
    if (NetworkSettingsBusy() || NetworkSettingsSimSlot() == target_slot) {
        return;
    }
    if (!IsCellNetwork() || !NetworkSettingsSupportsSim()) {
        ScreenShowToast(LangString_SETTINGS_NET_NOT_4G);
        return;
    }
    s_request = NetworkSettingsSwitchSim(target_slot);
    RefreshNetworkOptions();
    if (s_request) {
        s_shown_progress = NetworkSettingsProgress::Queued;
        s_shown_countdown = -1;
        char text[80];
        std::snprintf(text, sizeof(text), LangString_SETTINGS_NET_SWITCH_SIM_CFUN0_FMT, SimSlotName(target_slot));
        OpenOverlay(text);
    } else {
        ScreenShowToast(LangString_SETTINGS_NET_SIM_TASK_FAIL);
    }
}

static void PollNetworkSettings() {
    const auto now = lv_tick_get();
    if (now - s_wifi_update_ms >= 500) {
        s_wifi_update_ms = now;
        RefreshWifiPower(nullptr);
    }
    if (!s_request) {
        return;
    }
    if (s_request->completed.load(std::memory_order_acquire)) {
        // 板卡在重启后才改变活动网络，成功结果继续保持目标界面。
        if (s_request->operation == NetworkSettingsOperation::SwitchNetwork && s_request->ok) {
            return;
        }
        auto result = std::move(s_request);
        RefreshNetworkOptions();
        if (!result->ok) {
            CloseOverlay();
            ScreenShowToast(result->error);
        }
        return;
    }
    if (s_request->operation != NetworkSettingsOperation::SwitchSim) {
        return;
    }
    const auto progress = s_request->progress.load(std::memory_order_acquire);
    const int remaining = progress == NetworkSettingsProgress::Restart
                              ? static_cast<int>(std::max<int64_t>(0, s_request->reboot_at_us.load() - esp_timer_get_time()) / 1000000)
                              : -1;
    if (progress == s_shown_progress && remaining == s_shown_countdown) {
        return;
    }
    s_shown_progress = progress;
    s_shown_countdown = remaining;
    if (!s_ui.overlay) {
        OpenOverlay(LangString_SETTINGS_NET_CFUN0);
    }
    char text[160];
    if (progress == NetworkSettingsProgress::Select) {
        char command[64];
        std::snprintf(command, sizeof(command), "AT+ECSIMCFG=SimSlot,%d", s_request->target_slot);
        std::snprintf(text, sizeof(text), LangString_SETTINGS_NET_SWITCH_SIM_FMT, SimSlotName(s_request->target_slot), command);
        SetOverlayText(text);
    } else if (progress == NetworkSettingsProgress::Online) {
        SetOverlayText(LangString_SETTINGS_NET_CFUN1);
    } else if (progress == NetworkSettingsProgress::Restart) {
        char headline[64];
        std::snprintf(headline, sizeof(headline), LangString_SETTINGS_NET_SWITCHED_FMT, SimSlotName(s_request->target_slot));
        std::snprintf(text, sizeof(text), LangString_SETTINGS_NET_REBOOT_COUNT_FMT, headline, std::max(1, remaining));
        SetOverlayText(remaining > 0 ? text : LangString_SETTINGS_NET_REBOOTING);
    } else {
        SetOverlayText(LangString_SETTINGS_NET_CFUN0);
    }
}

static void OnSimExtClicked(lv_event_t* /*e*/) {
    ScheduleSimSwitch(kSimSlotExternal);
}

static void OnSimIntClicked(lv_event_t* /*e*/) {
    ScheduleSimSwitch(kSimSlotInternal);
}

static void BuildWifiConfigSection(lv_obj_t* page) {
    s_ui.wifi_power_btn = SettingsCreateToggle(page, "Wi-Fi", OnWifiPowerClicked);
    s_ui.wifi_cfg_box = lv_obj_get_parent(s_ui.wifi_power_btn);

    RefreshWifiPower(nullptr);
    SetWifiConfigSectionVisible(false);
}

[[maybe_unused]] static void BuildSimSection(lv_obj_t* page) {
    s_ui.sim_box = SettingsCreateSection(page, LangString_SETTINGS_NET_SIM_TITLE);

    auto* sim_list = SettingsCreateListGroup(s_ui.sim_box);
    s_ui.sim_ext_btn = SettingsCreateListRow(sim_list, LangString_SETTINGS_NET_SIM_EXT, "", OnSimExtClicked, 0, false);
    s_ui.sim_int_btn = SettingsCreateListRow(sim_list, LangString_SETTINGS_NET_SIM_INT, "", OnSimIntClicked, 0, false);

    SetSimSectionVisible(false);
}

void SettingsNetworkTab_BuildWifi(lv_obj_t* page) {
    s_ui = {};
    s_ui.root_scr = lv_obj_get_screen(page);
    s_page.Bind(page, {PollNetworkSettings, nullptr, SettingsNetworkTab_Reset, 100}, true);
    BuildWifiConfigSection(page);
    lv_obj_clear_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_HIDDEN);
    RefreshWifiPower(nullptr);
}

void SettingsNetworkTab_Reset() {
    s_page.Unbind();
    s_request.reset();
    CloseOverlay();
    s_ui = {};
}

const char* SettingsNetworkTab_CurrentName() {
    return IsCellNetwork() ? "4G" : "WiFi";
}

void SettingsNetworkTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_page.Bind(page, {PollNetworkSettings, nullptr, SettingsNetworkTab_Reset, 100}, true);
    s_ui.root_scr = lv_obj_get_screen(page);

    #if CONFIG_PRODUCT_GLOBAL
    s_request = NetworkSettingsCurrent();
    auto* group = SettingsCreateListGroup(page);
    SettingsCreateListRow(group, std::strcmp(LangCode, "zh-CN") == 0 ? "连接方式" : "Connection mode", "Wi-Fi");
    RefreshNetworkOptions();
    #else
    auto* mode_section = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "连接方式" : "Connection mode");
    auto* modes = SettingsCreateListGroup(mode_section);
    s_ui.wifi_btn = SettingsCreateListRow(modes, LangString_SETTINGS_NET_MODE_WIFI, "", OnNetworkOptionClicked,
                                         static_cast<intptr_t>(NetworkType::WIFI), false);
    s_ui.cell_btn = SettingsCreateListRow(modes, LangString_SETTINGS_NET_MODE_4G, "", OnNetworkOptionClicked,
                                         static_cast<intptr_t>(NetworkType::CELLULAR), false);
    SettingsCreateSectionHint(mode_section, LangString_SETTINGS_NET_REBOOT_HINT);

    BuildSimSection(page);
    ScheduleSimSlotQuery();
    RefreshNetworkOptions();
    #endif
    auto* info = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "网络信息" : "Network information");
    SettingsNetworkSignal_Build(info, true);
}
