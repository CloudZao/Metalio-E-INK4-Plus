#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/apps/settings/settings_frontlight_tab.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "config.h"
#include "ui/common/drawing/adapter/epd_refresh.h"

#if BOARD_HAS_VCOM
#include "business/diagnostics/panel_vcom.h"
#endif
#include "boards/common/display/frontlight.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* TAG = "SettingsFl";

struct SettingsFrontlightTabSettingsFrontlightUi {
    lv_obj_t* light_options[4] = {};        // 色温与关闭选项
    lv_obj_t* brightness_slider = nullptr;  // 亮度滑条
    lv_obj_t* brightness_label = nullptr;   // 目标亮度
    lv_obj_t* profiles[3] = {};             // 刷新模式选项
    lv_obj_t* refresh_hint = nullptr;       // 刷新模式说明
    lv_obj_t* depth_value = nullptr;        // 校准值，与标题同一行显示
    lv_obj_t* depth_group = nullptr;        // 有校准值时显示的信息外框
    lv_obj_t* depth_label = nullptr;        // 面板 VCOM 校准值标题
    lv_obj_t* depth_title = nullptr;        // 无校准值时的黑色深度分区标题
    lv_obj_t* depth_row = nullptr;          // 无有效校准记录时的档位列表
    lv_obj_t* depths[3] = {};               // 黑色深度选项行
    lv_obj_t* depth_hint = nullptr;         // 非默认档位的风险提示
    int depth_selected = -1;                // 已绘制档位，避免轮询时重复设置样式
    lv_timer_t* depth_timer = nullptr;      // 页面存活期间更新校准值
};

static SettingsFrontlightTabSettingsFrontlightUi s_ui;
static constexpr FrontlightCct kLightModes[] = {
    FrontlightCct::kCool,
    FrontlightCct::kWarm,
    FrontlightCct::kBoth,
    FrontlightCct::kOff,
};

static void RefreshDepth() {
    #if BOARD_HAS_VCOM
    if (!s_ui.depth_label || !s_ui.depth_row || !s_ui.depth_hint) {
        return;
    }
    const auto state = PanelVcomGetState();
    const bool calibrated = state.vcom_mv != 0;
    const bool fallback = !calibrated && state.revision != 0;
    lv_obj_set_flag(s_ui.depth_group, LV_OBJ_FLAG_HIDDEN, fallback);
    const char* section_title = fallback ? LangString_SETTINGS_SCREEN_BLACK_DEPTH_TITLE :
        std::strcmp(LangCode, "zh-CN") == 0 ? "屏幕校准" : "Screen calibration";
    if (std::strcmp(lv_label_get_text(s_ui.depth_title), section_title) != 0) {
        lv_label_set_text(s_ui.depth_title, section_title);
    }
    const char* title = std::strcmp(LangCode, "zh-CN") == 0 ? "VCOM 校准值" : "VCOM calibration";
    char value[48] = {};
    if (calibrated) {
        std::snprintf(value, sizeof(value), "%d mV", state.vcom_mv);
    } else if (!fallback) {
        std::snprintf(value, sizeof(value), "%s", LangString_SETTINGS_TEST_SIGNAL_READING);
    }
    if (std::strcmp(lv_label_get_text(s_ui.depth_label), title) != 0) {
        lv_label_set_text(s_ui.depth_label, title);
    }
    if (s_ui.depth_value && std::strcmp(lv_label_get_text(s_ui.depth_value), value) != 0) {
        lv_label_set_text(s_ui.depth_value, value);
    }
    if (fallback) {
        lv_obj_clear_flag(s_ui.depth_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.depth_row, LV_OBJ_FLAG_HIDDEN);
    }
    const int depth = static_cast<int>(EpdGetBlackDepth());
    if (s_ui.depth_selected != depth) {
        for (int i = 0; i < 3; ++i) {
            if (s_ui.depths[i]) {
                SettingsSetListRowCurrent(s_ui.depths[i], depth == i);
            }
        }
        s_ui.depth_selected = depth;
    }
    if (fallback && depth != 0) {
        lv_obj_clear_flag(s_ui.depth_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.depth_hint, LV_OBJ_FLAG_HIDDEN);
    }
    #endif
}

#if BOARD_HAS_VCOM
static void OnDepthClicked(lv_event_t* event) {
    const auto depth = static_cast<EpdBlackDepth>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (!EpdSetBlackDepth(depth) && !PanelVcomGetState().vcom_mv) {
        ESP_LOGE(TAG, "black depth change rejected level=%d", static_cast<int>(depth));
    }
    RefreshDepth();
}
#endif

static void RefreshOptions() {
    auto& light = Frontlight::GetInstance();
    for (int i = 0; i < 4; ++i) {
        if (s_ui.light_options[i]) {
            SettingsSetListRowCurrent(s_ui.light_options[i], light.cct() == kLightModes[i]);
        }
    }
    if (s_ui.brightness_slider) {
        lv_slider_set_value(s_ui.brightness_slider, light.target_brightness(), LV_ANIM_OFF);
        char text[48];
        std::snprintf(text, sizeof(text), LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT,
                      static_cast<int>(light.target_brightness()));
        lv_label_set_text(s_ui.brightness_label, text);
    }
    const auto profile = EpdGetRefreshProfile();
    for (int i = 0; i < 3; ++i) {
        if (s_ui.profiles[i]) {
            SettingsSetListRowCurrent(s_ui.profiles[i], static_cast<int>(profile) == i);
        }
    }
    if (s_ui.refresh_hint != nullptr) {
        const char* hints[] = {LangString_SETTINGS_SCREEN_REFRESH_CLEAR_HINT,
                               LangString_SETTINGS_SCREEN_REFRESH_BALANCED_HINT,
                               LangString_SETTINGS_SCREEN_REFRESH_FAST_HINT};
        lv_label_set_text(s_ui.refresh_hint, hints[static_cast<int>(profile)]);
    }
    RefreshDepth();
}

static lv_obj_t* OptionRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UiSx(12), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static void OnProfileClicked(lv_event_t* e) {
    const auto profile = static_cast<EpdRefreshProfile>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!EpdSetRefreshProfile(profile)) {
        ESP_LOGW("SettingsScreen", "operation=refresh_profile result=unavailable requested=%d",
                 static_cast<int>(profile));
    }
    RefreshOptions();
}

static void OnLightOptionClicked(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    light.SetCct(kLightModes[index]);
    if (kLightModes[index] != FrontlightCct::kOff && light.target_brightness() == 0) {
        light.SetBrightness(kFrontlightDefaultBrightness);
    }
    RefreshOptions();
    Frontlight::GetInstance().SavePreferences();
}

static void OnBrightnessChanged(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) {
        return;
    }
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        light.SetBrightness(static_cast<uint8_t>(lv_slider_get_value(s_ui.brightness_slider)));
        if (light.cct() == FrontlightCct::kOff) {
            light.SetWarmth(light.warmth());
        }
        RefreshOptions();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        Frontlight::GetInstance().SavePreferences();
    }
}

static void OnBrightnessStep(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) {
        return;
    }
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        light.SetBrightness(static_cast<uint8_t>(std::clamp<int>(light.target_brightness() + delta, 0, 100)));
        if (light.cct() == FrontlightCct::kOff) {
            light.SetWarmth(light.warmth());
        }
        RefreshOptions();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        Frontlight::GetInstance().SavePreferences();
    }
}

void SettingsFrontlightTab_Build(lv_obj_t* page) {
    SettingsFrontlightTab_Reset();
    ScreenFrontlightSetChangedCallback(SettingsFrontlightTab_Refresh);
    if (!BOARD_HAS_FRONTLIGHT) {
        RefreshOptions();
        return;
    }
    auto* color = SettingsCreateSection(page, LangString_SETTINGS_FRONTLIGHT_TITLE);
    const char* light_names[] = {LangString_SETTINGS_FRONTLIGHT_COOL, LangString_SETTINGS_FRONTLIGHT_WARM,
                                 LangString_SETTINGS_FRONTLIGHT_BOTH, LangString_SETTINGS_FRONTLIGHT_OFF};
    auto* light_group = SettingsCreateListGroup(color);
    for (int i = 0; i < 4; ++i) {
        s_ui.light_options[i] = SettingsCreateListRow(light_group, light_names[i], "", OnLightOptionClicked, i, false);
    }
    auto* brightness = SettingsCreateSection(page);
    s_ui.brightness_label = lv_label_create(brightness);
    lv_obj_set_width(s_ui.brightness_label, LV_PCT(100));
    lv_label_set_long_mode(s_ui.brightness_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.brightness_label, UiFont(), 0);
    auto* brightness_row = OptionRow(brightness);
    lv_obj_set_height(brightness_row, kSettingsBrightnessButtonH);
    lv_obj_set_style_pad_column(brightness_row, kSettingsBrightnessSliderGap, 0);
    lv_obj_set_flex_align(brightness_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int delta : {-1, 1}) {
        auto* step = lv_button_create(brightness_row);
        auto* label = lv_label_create(step);
        SettingsStyleBrightnessButton(step);
        lv_obj_set_size(step, kSettingsRoundButtonSize, kSettingsRoundButtonSize);
        lv_obj_set_style_radius(step, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_ext_click_area(step, UiSx(6));
        lv_obj_clear_flag(step, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(step);
        for (auto code : {LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(step, OnBrightnessStep, code, reinterpret_cast<void*>(static_cast<intptr_t>(delta)));
        }
        lv_label_set_text(label, delta < 0 ? LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC
                                           : LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_INC);
        lv_obj_center(label);
        if (delta > 0) {
            continue;
        }
        s_ui.brightness_slider = lv_slider_create(brightness_row);
        lv_obj_set_width(s_ui.brightness_slider, 0);
        lv_obj_set_flex_grow(s_ui.brightness_slider, 1);
        ScreenStyleProgressSlider(s_ui.brightness_slider);
        lv_obj_clear_flag(s_ui.brightness_slider, LV_OBJ_FLAG_SCROLLABLE);
        lv_slider_set_range(s_ui.brightness_slider, 0, 100);
        for (auto code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(s_ui.brightness_slider, OnBrightnessChanged, code, nullptr);
        }
    }
    RefreshOptions();
}

void SettingsDisplayTab_Build(lv_obj_t* page) {
    SettingsFrontlightTab_Reset();
    ScreenFrontlightSetChangedCallback(SettingsFrontlightTab_Refresh);

    auto* refresh = SettingsCreateSection(page, LangString_SETTINGS_SCREEN_REFRESH_TITLE);
    auto* profile_group = SettingsCreateListGroup(refresh);
    const char* profile_names[] = {LangString_SETTINGS_SCREEN_REFRESH_CLEAR,
                                   LangString_SETTINGS_SCREEN_REFRESH_BALANCED,
                                   LangString_SETTINGS_SCREEN_REFRESH_FAST};
    for (int i = 0; i < 3; ++i) {
        if (!EpdProfileSupported(static_cast<EpdRefreshProfile>(i))) {
            continue;
        }
        s_ui.profiles[i] = SettingsCreateListRow(profile_group, profile_names[i], "", OnProfileClicked, i, false);
    }
    s_ui.refresh_hint = lv_label_create(refresh);
    lv_label_set_text(s_ui.refresh_hint, LangString_SETTINGS_SCREEN_REFRESH_FAST_HINT);
    lv_obj_set_width(s_ui.refresh_hint, lv_pct(100));
    lv_label_set_long_mode(s_ui.refresh_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.refresh_hint, UiFont(UiFontRole::Caption), 0);
    lv_obj_set_style_text_color(s_ui.refresh_hint, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.refresh_hint, LV_OBJ_FLAG_CLICKABLE);
    #if BOARD_HAS_VCOM
    auto* depth_section = SettingsCreateSection(page);
    SettingsCreateSectionTitle(depth_section, LangString_SETTINGS_SCREEN_BLACK_DEPTH_TITLE);
    s_ui.depth_title = lv_obj_get_child(depth_section, -1);
    s_ui.depth_group = SettingsCreateListGroup(depth_section);
    auto* calibration = SettingsCreateListRow(s_ui.depth_group, "VCOM", "—");
    s_ui.depth_label = lv_obj_get_child(calibration, 0);
    s_ui.depth_value = lv_obj_get_child(calibration, 1);
    s_ui.depth_row = SettingsCreateListGroup(depth_section);
    const char* depth_names[] = {LangString_SETTINGS_SCREEN_BLACK_DEPTH_DEFAULT,
                                 LangString_SETTINGS_SCREEN_BLACK_DEPTH_MEDIUM,
                                 LangString_SETTINGS_SCREEN_BLACK_DEPTH_STRONG};
    for (int i = 0; i < 3; ++i) {
        s_ui.depths[i] = SettingsCreateListRow(s_ui.depth_row, depth_names[i], "", OnDepthClicked, i, false);
    }
    lv_obj_add_flag(s_ui.depth_row, LV_OBJ_FLAG_HIDDEN);
    s_ui.depth_hint = lv_label_create(depth_section);
    lv_label_set_text(s_ui.depth_hint, LangString_SETTINGS_SCREEN_BLACK_DEPTH_HINT);
    lv_obj_set_width(s_ui.depth_hint, lv_pct(100));
    lv_label_set_long_mode(s_ui.depth_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.depth_hint, UiFont(UiFontRole::Caption), 0);
    lv_obj_set_style_text_color(s_ui.depth_hint, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.depth_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.depth_hint, LV_OBJ_FLAG_HIDDEN);
    s_ui.depth_timer = lv_timer_create([](lv_timer_t*) { RefreshDepth(); }, 500, nullptr);
    lv_obj_add_event_cb(
        s_ui.depth_label,
        [](lv_event_t*) {
            if (s_ui.depth_timer) {
                lv_timer_delete(s_ui.depth_timer);
            }
            s_ui.depth_timer = nullptr;
            s_ui.depth_label = nullptr;
        },
        LV_EVENT_DELETE, nullptr);
    #endif
    RefreshOptions();
}

void SettingsFrontlightTab_Reset() {
    ScreenFrontlightSetChangedCallback(nullptr);
    if (s_ui.depth_timer) {
        lv_timer_delete(s_ui.depth_timer);
    }
    s_ui = {};
}

void SettingsFrontlightTab_Refresh() {
    RefreshOptions();
}
