#include "settings_frontlight_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "settings.h"
#include "settings_common.h"

#include <cstdio>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SettingsFl"

namespace {

constexpr const char* kNvsNs = "frontlight";
constexpr const char* kNvsKeyCct = "cct";
constexpr int kBrightnessStep = 5; // 亮度步进

struct SettingsFrontlightOption {
    FrontlightCct cct; // 色温组合
    const char* (*title)(); // 选项标题
    const char* (*current_text)(); // 当前选中文案
};

static const char* TitleCool() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_COOL;
}
static const char* TitleWarm() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_WARM;
}
static const char* TitleBoth() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_BOTH;
}
static const char* TitleOff() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_OFF;
}
static const char* CurCool() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_CURRENT_COOL;
}
static const char* CurWarm() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_CURRENT_WARM;
}
static const char* CurBoth() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_CURRENT_BOTH;
}
static const char* CurOff() {
    return Lang::Strings::SETTINGS_FRONTLIGHT_CURRENT_OFF;
}

static constexpr SettingsFrontlightOption kOptions[] = {
    {FrontlightCct::kCool, TitleCool, CurCool},
    {FrontlightCct::kWarm, TitleWarm, CurWarm},
    {FrontlightCct::kBoth, TitleBoth, CurBoth},
    {FrontlightCct::kOff, TitleOff, CurOff},
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

struct SettingsFrontlightUi {
    lv_obj_t* btns[kOptionCount] = {}; // 色温选项
    lv_obj_t* current_lbl = nullptr; // 当前色温文案
    lv_obj_t* brightness_lbl = nullptr; // 亮度文案
};

static SettingsFrontlightUi s_ui;

static FrontlightCct NormalizeCct(int value) {
    if (value < static_cast<int>(FrontlightCct::kWarm) ||
        value > static_cast<int>(FrontlightCct::kOff)) {
        return FrontlightCct::kOff;
    }
    return static_cast<FrontlightCct>(value);
}

static FrontlightCct CurrentCct() {
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        return FrontlightCct::kOff;
    }
    return fl.cct();
}

static const char* CurrentText(FrontlightCct cct) {
    for (int i = 0; i < kOptionCount; ++i) {
        if (kOptions[i].cct == cct) {
            return kOptions[i].current_text();
        }
    }
    return kOptions[0].current_text();
}

static uint8_t CurrentBrightness() {
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        return 0;
    }
    return fl.target_brightness();
}

static void RefreshBrightnessLabel() {
    if (s_ui.brightness_lbl == nullptr) {
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT,
                  static_cast<int>(CurrentBrightness()));
    lv_label_set_text(s_ui.brightness_lbl, buf);
}

static void RefreshOptions() {
    const FrontlightCct cct = CurrentCct();
    for (int i = 0; i < kOptionCount; ++i) {
        if (s_ui.btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0),
                                    kOptions[i].cct == cct);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, CurrentText(cct));
    }
    RefreshBrightnessLabel();
}

static void PersistCctTask(void* arg) {
    const int cct = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kNvsNs, true);
    settings.SetInt(kNvsKeyCct, cct);
    ESP_LOGI(TAG, "nvs save cct=%d", cct);
    vTaskDelete(nullptr);
}

static void ApplyCct(FrontlightCct cct) {
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        ESP_LOGW(TAG, "frontlight not ready");
        return;
    }
    fl.SetCct(cct);
    const int value = static_cast<int>(cct);
    if (xTaskCreate(PersistCctTask, "fl_cct_nvs", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(value)), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(fl_cct_nvs) failed");
    }
}

static void OnOptionClicked(lv_event_t* e) {
    const auto cct = NormalizeCct(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    if (CurrentCct() == cct) {
        return;
    }
    ApplyCct(cct);
    RefreshOptions();
}

static void OnBrightnessDelta(lv_event_t* e) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        return;
    }
    int next = static_cast<int>(fl.target_brightness()) + delta;
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    if (next == fl.target_brightness() && next == fl.brightness()) {
        return;
    }
    fl.SetBrightness(static_cast<uint8_t>(next), true);
    RefreshBrightnessLabel();
}

static lv_obj_t* CreateBrightnessButton(lv_obj_t* parent, const char* title, int delta) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSettingsBrightBtnW, kSettingsOptionH);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnBrightnessDelta, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(delta)));

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}

} // namespace

void SettingsFrontlightTab_Reset() {
    s_ui = {};
}

void SettingsFrontlightTab_Build(lv_obj_t* page) {
    s_ui = {};

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_FRONTLIGHT_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_FRONTLIGHT_HINT);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, kOptions[i].title(), OnOptionClicked,
            static_cast<intptr_t>(static_cast<int>(kOptions[i].cct)));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* bright_row = lv_obj_create(page);
    lv_obj_remove_style_all(bright_row);
    lv_obj_set_width(bright_row, lv_pct(100));
    lv_obj_set_height(bright_row, kSettingsOptionH);
    lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bright_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bright_row, LV_OBJ_FLAG_CLICKABLE);

    CreateBrightnessButton(bright_row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC,
                           -kBrightnessStep);

    s_ui.brightness_lbl = lv_label_create(bright_row);
    lv_label_set_text(s_ui.brightness_lbl, "");
    lv_obj_set_style_text_font(s_ui.brightness_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.brightness_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.brightness_lbl, LV_OBJ_FLAG_CLICKABLE);

    CreateBrightnessButton(bright_row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_INC,
                           kBrightnessStep);

    RefreshOptions();
}
