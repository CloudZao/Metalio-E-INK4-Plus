#include <cstring>
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_screen.h"

struct SettingsLangOption {
    const char* code;        // 语言码
    const char* const* title;  // 选项标题
};

static constexpr SettingsLangOption kLangOptions[] = {
    {"zh-CN", &LangString_SETTINGS_LANG_ZH_CN},
    {"en-US", &LangString_SETTINGS_LANG_EN_US},
};
static constexpr int kLangOptionCount = sizeof(kLangOptions) / sizeof(kLangOptions[0]);

struct SettingsLangUi {
    lv_obj_t* rows[kLangOptionCount] = {};  // 语言选项行
};

static SettingsLangUi s_ui;

static void RefreshLangOptions() {
    for (int i = 0; i < kLangOptionCount; ++i) {
        if (s_ui.rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.rows[i],
                                  std::strcmp(LangCode, kLangOptions[i].code) == 0);
        }
    }
}

static void OnLangOptionClicked(lv_event_t* e) {
    const char* code = static_cast<const char*>(lv_event_get_user_data(e));
    if (code == nullptr || std::strcmp(LangCode, code) == 0) {
        return;
    }
    // 内存先切；NVS 由 SetLanguage 异步落盘（勿在 LVGL 任务直接写 flash）
    LangSetLanguage(code, true);
    SettingsScreen::ReloadAppearance();
}

void SettingsLanguageTab_Build(lv_obj_t* page) {
    s_ui = {};

    auto* section = SettingsCreateSection(page, LangString_SETTINGS_LANG_TITLE);

    auto* group = SettingsCreateListGroup(section);
    for (int i = 0; i < kLangOptionCount; ++i) {
        s_ui.rows[i] = SettingsCreateListRow(group, *kLangOptions[i].title, "", OnLangOptionClicked,
                                               reinterpret_cast<intptr_t>(kLangOptions[i].code), false);
    }

    RefreshLangOptions();
}

void SettingsLanguageTab_Reset() {
    s_ui = {};
}
