#include <cstdio>
#include <cstring>
#include "assets/lang_charging.h"
#include "boards/common/power/cx25601n/charger_control.h"
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/settings_common.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ui_fonts.h"

static constexpr int kCurrentCount = sizeof(kChargeCurrentOptionsMa) / sizeof(kChargeCurrentOptionsMa[0]);

static constexpr int kMhzOptions[] = {
    BOARD_IDLE_CPU_LOW_MHZ,
    BOARD_IDLE_CPU_MID_MHZ,
    BOARD_IDLE_CPU_HIGH_MHZ,
};
static constexpr int kMhzCount = sizeof(kMhzOptions) / sizeof(kMhzOptions[0]);
static constexpr int kNetGraceCount =
    sizeof(kPowerPolicyNetGraceOptionsSec) / sizeof(kPowerPolicyNetGraceOptionsSec[0]);

static constexpr int kStandbyOffCount =
    sizeof(kPowerPolicyStandbyOffOptionsSec) / sizeof(kPowerPolicyStandbyOffOptionsSec[0]);

struct SettingsPowerTabPowerUi {
    lv_obj_t* charging_rows[kCurrentCount] = {};
    lv_obj_t* charging_error = nullptr;
    lv_obj_t* mhz_rows[kMhzCount] = {};
    lv_obj_t* grace_rows[kNetGraceCount] = {};
    lv_obj_t* standby_off_rows[kStandbyOffCount] = {};
};

static SettingsPowerTabPowerUi s_ui;

static const char* StandbyOffOptionTitle(int sec, char* buffer, size_t size) {
    switch (sec) {
        case 3600:
            return LangString_SETTINGS_POWER_1_HOUR;
        case 10800:
            return LangString_SETTINGS_POWER_3_HOURS;
        case 86400:
            return LangString_SETTINGS_POWER_24_HOURS;
        case 1800:
            return LangString_SETTINGS_POWER_30_MIN;
        default:
            std::snprintf(buffer, size, LangString_SETTINGS_POWER_SECONDS_FMT, sec);
            return buffer;
    }
}

static const char* SecOptionTitle(int sec, char* buffer, size_t size) {
    switch (sec) {
        case 0:
            return LangString_SETTINGS_POWER_OFF_NEVER;
        case 30:
            return LangString_SETTINGS_POWER_30_SEC;
        case 60:
            return LangString_SETTINGS_POWER_60_SEC;
        case 120:
            return LangString_SETTINGS_POWER_120_SEC;
        default:
            std::snprintf(buffer, size, LangString_SETTINGS_POWER_SECONDS_FMT, sec);
            return buffer;
    }
}

static void RefreshCharging() {
    const int current = ChargerControl_GetChargeCurrentMa();
    for (int i = 0; i < kCurrentCount; ++i) {
        SettingsSetListRowCurrent(s_ui.charging_rows[i], kChargeCurrentOptionsMa[i] == current);
    }
}

static void OnCurrentClicked(lv_event_t* e) {
    const int ma = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const bool ok = ChargerControl_SetChargeCurrentMa(ma);
    lv_label_set_text(s_ui.charging_error, ok ? "" : LangString_SETTINGS_CHARGING_FAILED);
    lv_obj_set_flag(s_ui.charging_error, LV_OBJ_FLAG_HIDDEN, ok);
    RefreshCharging();
}

static void BuildCharging(lv_obj_t* page) {
    SettingsCreateSectionTitle(page, LangString_SETTINGS_CHARGING_CURRENT);
    auto* group = SettingsCreateListGroup(page);
    for (int i = 0; i < kCurrentCount; ++i) {
        const int ma = kChargeCurrentOptionsMa[i];
        char title[24];
        std::snprintf(title, sizeof(title), "%d mA", ma);
        s_ui.charging_rows[i] = SettingsCreateListRow(group, title, "", OnCurrentClicked, kChargeCurrentOptionsMa[i], false);
    }
    s_ui.charging_error = lv_label_create(page);
    lv_label_set_text(s_ui.charging_error, "");
    lv_obj_add_flag(s_ui.charging_error, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_ui.charging_error, UiFont(), 0);
    lv_obj_set_width(s_ui.charging_error, lv_pct(100));
    lv_label_set_long_mode(s_ui.charging_error, LV_LABEL_LONG_WRAP);
    RefreshCharging();
}

static void RefreshOptions() {
    auto& pp = PowerPolicy::GetInstance();
    const int mhz = pp.GetIdleCpuMhz();
    const int grace_s = pp.GetNetGraceSec();
    const int standby_off_s = pp.GetStandbyToShutdownSec();

    for (int i = 0; i < kMhzCount; ++i) {
        if (s_ui.mhz_rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.mhz_rows[i], kMhzOptions[i] == mhz);
        }
    }
    for (int i = 0; i < kNetGraceCount; ++i) {
        if (s_ui.grace_rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.grace_rows[i], kPowerPolicyNetGraceOptionsSec[i] == grace_s);
        }
    }
    for (int i = 0; i < kStandbyOffCount; ++i) {
        if (s_ui.standby_off_rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.standby_off_rows[i], kPowerPolicyStandbyOffOptionsSec[i] == standby_off_s);
        }
    }
}

static void OnMhzClicked(lv_event_t* e) {
    const int mhz = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (PowerPolicy::GetInstance().GetIdleCpuMhz() == mhz) {
        return;
    }
    PowerPolicy::GetInstance().SetIdleCpuMhz(mhz);
    RefreshOptions();
}

static void OnNetGraceClicked(lv_event_t* e) {
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (PowerPolicy::GetInstance().GetNetGraceSec() == sec) {
        return;
    }
    PowerPolicy::GetInstance().SetNetGraceSec(sec);
    RefreshOptions();
}

static void OnStandbyOffClicked(lv_event_t* e) {
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    PowerPolicy::GetInstance().SetStandbyToShutdownSec(sec);
    RefreshOptions();
}

void SettingsPowerTab_Build(lv_obj_t* page) {
    s_ui = {};
    lv_obj_set_style_pad_row(page, kSettingsSectionGap, 0);
    auto* cpu = SettingsCreateSection(page, LangString_SETTINGS_POWER_IDLE_MHZ_TITLE);
    auto* cpu_group = SettingsCreateListGroup(cpu);
    for (int i = 0; i < kMhzCount; ++i) {
        char title[16];
        std::snprintf(title, sizeof(title), "%d", kMhzOptions[i]);
        s_ui.mhz_rows[i] = SettingsCreateListRow(cpu_group, title, "", OnMhzClicked, kMhzOptions[i], false);
    }
    SettingsCreateSectionHint(cpu, LangString_SETTINGS_POWER_IDLE_MHZ_HINT);
    auto* network = SettingsCreateSection(page, LangString_SETTINGS_POWER_NET_GRACE_TITLE);
    auto* network_group = SettingsCreateListGroup(network);
    for (int i = 0; i < kNetGraceCount; ++i) {
        char title[32];
        const int seconds = kPowerPolicyNetGraceOptionsSec[i];
        s_ui.grace_rows[i] = SettingsCreateListRow(network_group, SecOptionTitle(seconds, title, sizeof(title)), "",
                                                   OnNetGraceClicked, seconds, false);
    }
    SettingsCreateSectionHint(network, LangString_SETTINGS_POWER_NET_GRACE_HINT);
    auto* shutdown = SettingsCreateSection(page, LangString_SETTINGS_POWER_OFF_TITLE);
    auto* shutdown_group = SettingsCreateListGroup(shutdown);
    for (int i = 0; i < kStandbyOffCount; ++i) {
        char title[32];
        const int seconds = kPowerPolicyStandbyOffOptionsSec[i];
        s_ui.standby_off_rows[i] = SettingsCreateListRow(shutdown_group,
            StandbyOffOptionTitle(seconds, title, sizeof(title)), "", OnStandbyOffClicked, seconds, false);
    }
    SettingsCreateSectionHint(shutdown, LangString_SETTINGS_POWER_OFF_HINT);
    BuildCharging(SettingsCreateSection(page, nullptr));
    auto* standby = SettingsCreateSection(page, LangString_SETTINGS_POWER_STANDBY_TITLE);
    auto* standby_group = SettingsCreateListGroup(standby);
    char duration[32];
    const bool minutes = BOARD_IDLE_TO_STANDBY_SECONDS % 60 == 0;
    const char* format = minutes ? (std::strcmp(LangCode, "zh-CN") == 0 ? "%d 分钟" : "%d min") :
                                  LangString_SETTINGS_POWER_SECONDS_FMT;
    std::snprintf(duration, sizeof(duration), format,
                  minutes ? BOARD_IDLE_TO_STANDBY_SECONDS / 60 : BOARD_IDLE_TO_STANDBY_SECONDS);
    auto* standby_row = SettingsCreateListRow(standby_group, duration, "", nullptr, 0, false);
    SettingsSetListRowCurrent(standby_row, true);
    char hint[128];
    std::snprintf(hint, sizeof(hint), LangString_SETTINGS_POWER_STANDBY_HINT, duration);
    SettingsCreateSectionHint(standby, hint);
    RefreshOptions();
}

void SettingsPowerTab_Reset() {
    s_ui = {};
}
