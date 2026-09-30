#include "settings_language_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "settings_common.h"
#include "settings_screen.h"

#include <cstring>

struct SettingsLangOption {
    const char* code; // 语言码
    const char* (*title)(); // 选项标题
};

static const char* TitleZh() {
    return Lang::Strings::SETTINGS_LANG_ZH_CN;
}
static const char* TitleEn() {
    return Lang::Strings::SETTINGS_LANG_EN_US;
}

static constexpr SettingsLangOption kLangOptions[] = {
    {"zh-CN", TitleZh},
    {"en-US", TitleEn},
};
static constexpr int kLangOptionCount = sizeof(kLangOptions) / sizeof(kLangOptions[0]);

struct SettingsLangUi {
    lv_obj_t* btns[kLangOptionCount] = {}; // 选项按钮
    lv_obj_t* current_lbl = nullptr; // 当前语言文案
};

static SettingsLangUi s_ui;

static const char* CurrentLabelText() {
    if (std::strcmp(Lang::CODE, "zh-CN") == 0) {
        return Lang::Strings::SETTINGS_LANG_CURRENT_ZH;
    }
    return Lang::Strings::SETTINGS_LANG_CURRENT_EN;
}

static void RefreshLangOptions() {
    for (int i = 0; i < kLangOptionCount; ++i) {
        if (s_ui.btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0),
                                    std::strcmp(Lang::CODE, kLangOptions[i].code) == 0);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, CurrentLabelText());
    }
}

static void OnLangOptionClicked(lv_event_t* e) {
    const char* code = static_cast<const char*>(lv_event_get_user_data(e));
    if (code == nullptr || std::strcmp(Lang::CODE, code) == 0) {
        return;
    }
    // 内存先切；NVS 由 SetLanguage 异步落盘（勿在 LVGL 任务直接写 flash）
    Lang::SetLanguage(code, true);
    SettingsScreen::ReloadAfterLanguageChange();
}

void SettingsLanguageTab_Reset() {
    s_ui = {};
}

void SettingsLanguageTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_LANG_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_LANG_HINT);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kLangOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, kLangOptions[i].title(), OnLangOptionClicked,
            reinterpret_cast<intptr_t>(kLangOptions[i].code));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshLangOptions();
}
