#include "settings_frontlight_tab.h"

#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "application.h"
#include "epd_refresh.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "settings.h"
#include "settings_common.h"

#include <algorithm>
#include <cstdio>
#include <esp_log.h>

namespace {

struct SettingsFrontlightUi {
    lv_obj_t* light_options[4] = {}; // 色温与关闭选项
    lv_obj_t* brightness_slider = nullptr; // 亮度滑条
    lv_obj_t* brightness_label = nullptr; // 目标亮度
    lv_obj_t* profiles[4] = {}; // 刷新模式选项
    lv_obj_t* refresh_hint = nullptr; // 刷新模式说明
    lv_obj_t* depths[3] = {}; // 黑色深度选项
    lv_obj_t* depth_hint = nullptr; // 黑色深度说明
};

static SettingsFrontlightUi s_ui;
constexpr FrontlightCct kLightModes[] = {
    FrontlightCct::kCool, FrontlightCct::kWarm, FrontlightCct::kBoth, FrontlightCct::kOff,
};

static void SaveFrontlight() {
    auto& light = Frontlight::GetInstance();
    const int brightness = light.target_brightness();
    const int warmth = light.warmth();
    const int cct = static_cast<int>(light.cct());
    if (!Application::GetInstance().Schedule([brightness, warmth, cct] {
        Settings settings("frontlight", true);
        settings.SetInt("brightness", brightness);
        settings.SetInt("warmth", warmth);
        settings.SetInt("cct", cct);
    }, Application::TaskDomain::Storage)) {
        ESP_LOGW("SettingsFl", "frontlight save request failed");
    }
}

static void RefreshOptions() {
    auto& light = Frontlight::GetInstance();
    for (int i = 0; i < 4; ++i) {
        if (s_ui.light_options[i]) SettingsStyleSelectable(s_ui.light_options[i],
            lv_obj_get_child(s_ui.light_options[i], 0), light.cct() == kLightModes[i]);
    }
    if (s_ui.brightness_slider) {
        lv_slider_set_value(s_ui.brightness_slider, light.target_brightness(), LV_ANIM_OFF);
        char text[48];
        std::snprintf(text, sizeof(text), Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT,
                      static_cast<int>(light.target_brightness()));
        lv_label_set_text(s_ui.brightness_label, text);
    }
    const auto profile = EpdGetRefreshProfile();
    for (int i=0;i<4;++i) {
        if (s_ui.profiles[i]) SettingsStyleSelectable(s_ui.profiles[i],
            lv_obj_get_child(s_ui.profiles[i],0),static_cast<int>(profile)==i);
    }
    if (s_ui.refresh_hint != nullptr) {
        const char* hints[]={Lang::Strings::SETTINGS_SCREEN_REFRESH_CLEAR_HINT,
            Lang::Strings::SETTINGS_SCREEN_REFRESH_BALANCED_HINT,Lang::Strings::SETTINGS_SCREEN_REFRESH_FAST_HINT,
            Lang::Strings::SETTINGS_SCREEN_REFRESH_EXTREME_HINT};
        lv_label_set_text(s_ui.refresh_hint,hints[static_cast<int>(profile)]);
    }
    const int depth=static_cast<int>(EpdGetBlackDepth());
    for (int i=0;i<3;++i) {
        if (s_ui.depths[i]) SettingsStyleSelectable(s_ui.depths[i],lv_obj_get_child(s_ui.depths[i],0),depth==i);
    }
    if (s_ui.depth_hint) {
        if (depth==0) lv_obj_add_flag(s_ui.depth_hint,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_ui.depth_hint,LV_OBJ_FLAG_HIDDEN);
    }

}

static void OnDepthClicked(lv_event_t* e) {
    const auto depth=static_cast<EpdBlackDepth>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (EpdSetBlackDepth(depth)) RefreshOptions();
}
static lv_obj_t* OptionRow(lv_obj_t* parent) {
    auto* row=lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row,lv_pct(100),LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row,UiSx(12),0);
    lv_obj_clear_flag(row,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
    return row;
}
static void FitRowButton(lv_obj_t* button) {
    lv_obj_set_width(button,0);
    lv_obj_set_flex_grow(button,1);
}

static void OnProfileClicked(lv_event_t* e) {
    const auto profile=static_cast<EpdRefreshProfile>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    EpdSetRefreshProfile(profile);
    RefreshOptions();
}

static void OnLightOptionClicked(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) return;
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    light.SetCct(kLightModes[index]);
    if (kLightModes[index] != FrontlightCct::kOff && light.target_brightness() == 0)
        light.SetBrightness(40);
    RefreshOptions();
    SaveFrontlight();
}

static void OnBrightnessChanged(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) return;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        light.SetBrightness(static_cast<uint8_t>(lv_slider_get_value(s_ui.brightness_slider)));
        if (light.cct() == FrontlightCct::kOff) light.SetWarmth(light.warmth());
        RefreshOptions();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SaveFrontlight();
    }
}

static void OnBrightnessStep(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) return;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        light.SetBrightness(static_cast<uint8_t>(std::clamp<int>(light.target_brightness() + delta, 0, 100)));
        if (light.cct() == FrontlightCct::kOff) light.SetWarmth(light.warmth());
        RefreshOptions();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SaveFrontlight();
    }
}

} // namespace

void SettingsFrontlightTab_Refresh() {
    RefreshOptions();
}

void SettingsFrontlightTab_Reset() {
    s_ui = {};
}

void SettingsFrontlightTab_Build(lv_obj_t* page) {
    s_ui = {};

    auto* refresh_title=lv_label_create(page);
    lv_label_set_text(refresh_title,Lang::Strings::SETTINGS_SCREEN_REFRESH_TITLE);
    lv_obj_set_style_text_font(refresh_title,fontpack_lv_font_ui(),0);
    lv_obj_set_style_text_color(refresh_title,lv_color_black(),0);
    lv_obj_t* profile_row=nullptr;
    const char* profile_names[]={Lang::Strings::SETTINGS_SCREEN_REFRESH_CLEAR,
        Lang::Strings::SETTINGS_SCREEN_REFRESH_BALANCED,Lang::Strings::SETTINGS_SCREEN_REFRESH_FAST,
        Lang::Strings::SETTINGS_SCREEN_REFRESH_EXTREME};
    for (int i=0;i<4;++i) {
        if (i%2==0) profile_row=OptionRow(page);
        s_ui.profiles[i]=SettingsCreateSelectableOption(profile_row,profile_names[i],OnProfileClicked,i);
    }
    for (auto* button:s_ui.profiles) FitRowButton(button);
    s_ui.refresh_hint = lv_label_create(page);
    lv_label_set_text(s_ui.refresh_hint, Lang::Strings::SETTINGS_SCREEN_REFRESH_FAST_HINT);
    lv_obj_set_width(s_ui.refresh_hint, lv_pct(100));
    lv_label_set_long_mode(s_ui.refresh_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.refresh_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.refresh_hint, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.refresh_hint, LV_OBJ_FLAG_CLICKABLE);
    auto* refresh_separator=lv_obj_create(page);
    lv_obj_remove_style_all(refresh_separator);
    lv_obj_set_size(refresh_separator,lv_pct(100),kSettingsSplitLineW);
    lv_obj_set_style_bg_color(refresh_separator,lv_color_black(),0);
    lv_obj_set_style_bg_opa(refresh_separator,LV_OPA_COVER,0);
    lv_obj_clear_flag(refresh_separator,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(refresh_separator,LV_OBJ_FLAG_SCROLLABLE);
    auto* depth_title=lv_label_create(page);
    lv_label_set_text(depth_title,Lang::Strings::SETTINGS_SCREEN_BLACK_DEPTH_TITLE);
    lv_obj_set_style_text_font(depth_title,fontpack_lv_font_ui(),0);
    lv_obj_set_style_text_color(depth_title,lv_color_black(),0);
    auto* depth_row=OptionRow(page);
    const char* depth_names[]={Lang::Strings::SETTINGS_SCREEN_BLACK_DEPTH_DEFAULT,
        Lang::Strings::SETTINGS_SCREEN_BLACK_DEPTH_MEDIUM,Lang::Strings::SETTINGS_SCREEN_BLACK_DEPTH_STRONG};
    for (int i=0;i<3;++i) {
        s_ui.depths[i]=SettingsCreateSelectableOption(depth_row,depth_names[i],OnDepthClicked,i);
        FitRowButton(s_ui.depths[i]);
    }
    s_ui.depth_hint=lv_label_create(page);
    lv_label_set_text(s_ui.depth_hint,Lang::Strings::SETTINGS_SCREEN_BLACK_DEPTH_HINT);
    lv_obj_set_width(s_ui.depth_hint,lv_pct(100));
    lv_label_set_long_mode(s_ui.depth_hint,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.depth_hint,fontpack_lv_font_ui(),0);
    lv_obj_set_style_text_color(s_ui.depth_hint,lv_color_black(),0);
    lv_obj_clear_flag(s_ui.depth_hint,LV_OBJ_FLAG_CLICKABLE);
    auto* separator = lv_obj_create(page);
    lv_obj_remove_style_all(separator);
    lv_obj_set_size(separator, lv_pct(100), kSettingsSplitLineW);
    lv_obj_set_style_bg_color(separator, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);
    auto* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_FRONTLIGHT_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    const char* light_names[] = {Lang::Strings::SETTINGS_FRONTLIGHT_COOL,
        Lang::Strings::SETTINGS_FRONTLIGHT_WARM, Lang::Strings::SETTINGS_FRONTLIGHT_BOTH,
        Lang::Strings::SETTINGS_FRONTLIGHT_OFF};
    lv_obj_t* light_row = nullptr;
    for (int i = 0; i < 4; ++i) {
        if (i % 2 == 0) light_row = OptionRow(page);
        s_ui.light_options[i] = SettingsCreateSelectableOption(light_row, light_names[i], OnLightOptionClicked, i);
        FitRowButton(s_ui.light_options[i]);
    }
    s_ui.brightness_label = lv_label_create(page);
    lv_obj_set_style_text_font(s_ui.brightness_label, fontpack_lv_font_ui(), 0);
    auto* brightness_row = OptionRow(page);
    lv_obj_set_height(brightness_row, kSettingsOptionH);
    lv_obj_set_flex_align(brightness_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int delta : {-1, 1}) {
        auto* step = lv_button_create(brightness_row);
        auto* label = lv_label_create(step);
        SettingsStyleBrightnessButton(step);
        lv_obj_clear_flag(step, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(step);
        for (auto code : {LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(step, OnBrightnessStep, code, reinterpret_cast<void*>(static_cast<intptr_t>(delta)));
        lv_label_set_text(label, delta < 0 ? Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC :
                                           Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_INC);
        lv_obj_center(label);
        if (delta > 0) continue;
        s_ui.brightness_slider = lv_slider_create(brightness_row);
        lv_obj_set_width(s_ui.brightness_slider, 0);
        lv_obj_set_flex_grow(s_ui.brightness_slider, 1);
        lv_obj_set_height(s_ui.brightness_slider, UiSy(16));
        lv_obj_clear_flag(s_ui.brightness_slider, LV_OBJ_FLAG_SCROLLABLE);
        lv_slider_set_range(s_ui.brightness_slider, 0, 100);
        for (auto code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(s_ui.brightness_slider, OnBrightnessChanged, code, nullptr);
    }
    RefreshOptions();
}
