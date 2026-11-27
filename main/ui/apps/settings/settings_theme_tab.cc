#include "assets/lang_config.h"
#include "assets/lang_pixel.h"
#include "ui/apps/settings/settings_tabs.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/home/home_screen.h"

struct SettingsThemeOption {
    int style;                      // 卡片样式枚举
    const char* const* title;         // 语言资源槽，切换语言后按需读取
};

static constexpr SettingsThemeOption kThemeOptions[] = {
    {HomeScreen::kCardStyleWhite, &LangString_SETTINGS_THEME_WHITE},
    {HomeScreen::kCardStyleGray, &LangString_SETTINGS_THEME_GRAY},
    {HomeScreen::kCardStyleBorder, &LangString_SETTINGS_THEME_BORDER},
    {HomeScreen::kCardStyleSlash, &LangString_SETTINGS_THEME_SLASH},
    {HomeScreen::kCardStylePixel, &LangString_SETTINGS_THEME_PIXEL},
};
static constexpr int kThemeOptionCount = sizeof(kThemeOptions) / sizeof(kThemeOptions[0]);

struct SettingsThemeUi {
    lv_obj_t* rows[kThemeOptionCount] = {};  // 样式选项行
};

static SettingsThemeUi s_ui;

static void RefreshThemeOptions() {
    const int style = HomeScreen::LoadCardStyle();
    for (int i = 0; i < kThemeOptionCount; ++i) {
        if (s_ui.rows[i] != nullptr) {
            SettingsSetListRowCurrent(s_ui.rows[i], kThemeOptions[i].style == style);
        }
    }
}

static void OnThemeOptionClicked(lv_event_t* e) {
    const int style = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (HomeScreen::LoadCardStyle() == style) {
        return;
    }
    HomeScreen::SaveCardStyle(style);
    RefreshThemeOptions();
}

void SettingsThemeTab_Build(lv_obj_t* page) {
    s_ui = {};
    // 界面明暗暂不开放入口，保留 UiTheme 能力供后续接入。

    auto* section = SettingsCreateSection(page, LangString_SETTINGS_THEME_TITLE);

    auto* group = SettingsCreateListGroup(section);
    for (int i = 0; i < kThemeOptionCount; ++i) {
        s_ui.rows[i] = SettingsCreateListRow(group, *kThemeOptions[i].title, "", OnThemeOptionClicked,
                                              static_cast<intptr_t>(kThemeOptions[i].style), false);
    }

    RefreshThemeOptions();
}

void SettingsThemeTab_Reset() {
    s_ui = {};
}
