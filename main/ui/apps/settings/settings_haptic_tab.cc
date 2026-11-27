#include <cstring>
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "assets/lang_haptic.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/haptic_feedback.h"

struct SettingsHapticIntensityOption {
    int intensity;                  // 0 弱 / 1 中 / 2 强
    const char* const* title;         // 语言资源槽，切换语言后按需读取
};

static constexpr SettingsHapticIntensityOption kIntensityOptions[] = {
    {0, &LangString_SETTINGS_HAPTIC_WEAK},
    {1, &LangString_SETTINGS_HAPTIC_MEDIUM},
    {2, &LangString_SETTINGS_HAPTIC_STRONG},
};
static constexpr int kIntensityOptionCount = sizeof(kIntensityOptions) / sizeof(kIntensityOptions[0]);

struct SettingsHapticUi {
    lv_obj_t* toggle = nullptr;                     // 开关控件
    lv_obj_t* intensity_rows[kIntensityOptionCount] = {};  // 强度选项行
};

static SettingsHapticUi s_ui;

static void RefreshOptions() {
    const bool enabled = HapticIsEnabled();
    if (s_ui.toggle) {
        lv_obj_set_state(s_ui.toggle, LV_STATE_CHECKED, enabled);
    }

    const int intensity = HapticGetIntensity();
    for (int i = 0; i < kIntensityOptionCount; ++i) {
        if (s_ui.intensity_rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.intensity_rows[i], kIntensityOptions[i].intensity == intensity);
        }
    }
}

static void OnOptionClicked(lv_event_t* e) {
    const bool enable = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    if (HapticIsEnabled() == enable) {
        return;
    }
    HapticSetEnabled(enable);
    RefreshOptions();
    // 关→开时 AttachClick 仍读到旧值不会震，此处补一次确认反馈
    if (enable) {
        HapticPulseIfEnabled();
    }
}

static void OnIntensityClicked(lv_event_t* e) {
    const int intensity = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (HapticGetIntensity() == intensity) {
        return;
    }
    HapticSetIntensity(intensity);
    RefreshOptions();
}

void SettingsHapticTab_Build(lv_obj_t* page) {
    s_ui = {};

    auto* toggle_section = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "震动开关" : "Haptic switch");
    auto* group = SettingsCreateListGroup(toggle_section);
    s_ui.toggle = SettingsCreateToggle(group, LangString_SETTINGS_HAPTIC_TITLE, OnOptionClicked);

    auto* section = SettingsCreateSection(page, LangString_SETTINGS_HAPTIC_INTENSITY_TITLE);

    auto* options = SettingsCreateListGroup(section);
    for (int i = 0; i < kIntensityOptionCount; ++i) {
        s_ui.intensity_rows[i] = SettingsCreateListRow(options, *kIntensityOptions[i].title, "", OnIntensityClicked,
                                                        static_cast<intptr_t>(kIntensityOptions[i].intensity), false);
    }

    RefreshOptions();
}

void SettingsHapticTab_Reset() {
    s_ui = {};
}
