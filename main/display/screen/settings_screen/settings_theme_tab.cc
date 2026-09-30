#include "settings_theme_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "home_screen/home_screen.h"
#include "settings_common.h"

struct SettingsThemeOption {
    int style; // 卡片样式枚举
    const char* (*title)(); // 选项标题
    const char* (*current_text)(); // 当前选中文案
};

static const char* ThemeTitleWhite() {
    return Lang::Strings::SETTINGS_THEME_WHITE;
}
static const char* ThemeTitleGray() {
    return Lang::Strings::SETTINGS_THEME_GRAY;
}
static const char* ThemeTitleBorder() {
    return Lang::Strings::SETTINGS_THEME_BORDER;
}
static const char* ThemeTitleSlash() {
    return Lang::Strings::SETTINGS_THEME_SLASH;
}
static const char* ThemeCurWhite() {
    return Lang::Strings::SETTINGS_THEME_CURRENT_WHITE;
}
static const char* ThemeCurGray() {
    return Lang::Strings::SETTINGS_THEME_CURRENT_GRAY;
}
static const char* ThemeCurBorder() {
    return Lang::Strings::SETTINGS_THEME_CURRENT_BORDER;
}
static const char* ThemeCurSlash() {
    return Lang::Strings::SETTINGS_THEME_CURRENT_SLASH;
}

static constexpr SettingsThemeOption kThemeOptions[] = {
    {HomeScreen::kCardStyleWhite, ThemeTitleWhite, ThemeCurWhite},
    {HomeScreen::kCardStyleGray, ThemeTitleGray, ThemeCurGray},
    {HomeScreen::kCardStyleBorder, ThemeTitleBorder, ThemeCurBorder},
    {HomeScreen::kCardStyleSlash, ThemeTitleSlash, ThemeCurSlash},
};
static constexpr int kThemeOptionCount = sizeof(kThemeOptions) / sizeof(kThemeOptions[0]);

struct SettingsThemeUi {
    lv_obj_t* btns[kThemeOptionCount] = {}; // 选项按钮
    lv_obj_t* current_lbl = nullptr; // 当前主题文案
};

static SettingsThemeUi s_ui;

static const char* ThemeCurrentText(int style) {
    for (int i = 0; i < kThemeOptionCount; ++i) {
        if (kThemeOptions[i].style == style) {
            return kThemeOptions[i].current_text();
        }
    }
    return kThemeOptions[0].current_text();
}

static void RefreshThemeOptions() {
    const int style = HomeScreen::LoadCardStyle();
    for (int i = 0; i < kThemeOptionCount; ++i) {
        if (s_ui.btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0),
                                    kThemeOptions[i].style == style);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, ThemeCurrentText(style));
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

void SettingsThemeTab_Reset() {
    s_ui = {};
}

void SettingsThemeTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_THEME_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_THEME_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kThemeOptionCount; ++i) {
        s_ui.btns[i] =
            SettingsCreateSelectableOption(page, kThemeOptions[i].title(), OnThemeOptionClicked,
                                           static_cast<intptr_t>(kThemeOptions[i].style));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshThemeOptions();
}
