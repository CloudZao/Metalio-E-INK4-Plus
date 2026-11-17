#include "settings_test_margin_screen.h"

#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "epdiy.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_epdiy.h"
#include "power_policy.h"
#include "screen_common.h"
#include "screen_text.h"
#include "settings.h"
#include "settings_common.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <esp_log.h>

static constexpr const char* TAG = "MarginTest";
static lv_obj_t* s_screen = nullptr;
static lv_obj_t* s_values[kDisplayUiOffsetCount] = {}; // 四个方向的当前平移量
static lv_obj_t* s_sliders[kDisplayUiOffsetCount] = {}; // 四个方向的独立滑条
static uint8_t s_dirty = 0; // 尚未提交保存的方向位图
static bool s_standby = false; // 待机及其底图捕获期间暂停预览
static lv_obj_t* s_background = nullptr; // 仅覆盖逻辑可绘制区域的校准底图

static const lv_image_dsc_t* BackgroundImage() {
    extern const uint8_t start[] asm("_binary_margin_calibration_png_start");
    extern const uint8_t end[] asm("_binary_margin_calibration_png_end");
    static const lv_image_dsc_t image = [] {
        lv_image_dsc_t value{};
        value.header.magic = LV_IMAGE_HEADER_MAGIC;
        value.header.cf = LV_COLOR_FORMAT_RAW;
        value.data = start;
        value.data_size = end - start;
        return value;
    }();
    return &image;
}

enum class MarginNotice { None, Set, Reset };
struct MarginSaveResult {
    uint32_t epoch; // 发起操作时的页面输入代次
    MarginNotice notice; // 操作对应的提示
    bool success; // 整数写入和提交均成功
};

static void ShowSaveResult(void* context) {
    const auto result = *static_cast<MarginSaveResult*>(context);
    delete static_cast<MarginSaveResult*>(context);
    if (s_screen == nullptr || s_screen != lv_screen_active() || s_standby || result.epoch != VkKey_InputEpoch()) return;
    ScreenShowToast(!result.success ? Lang::Strings::SETTINGS_SCREEN_MARGIN_SAVE_FAILED :
        result.notice == MarginNotice::Reset ? Lang::Strings::SETTINGS_SCREEN_MARGIN_RESET_OK :
                                              Lang::Strings::SETTINGS_SCREEN_MARGIN_SET_OK);
}

static void PostSaveResult(uint32_t epoch, MarginNotice notice, bool success) {
    if (notice == MarginNotice::None) return;
    auto* result = new(std::nothrow) MarginSaveResult{epoch, notice, success};
    if (result == nullptr) {
        ESP_LOGE(TAG, "margin result allocation failed");
        return;
    }
    if (!ScreenLvAsync(ShowSaveResult, result)) {
        delete result;
        ESP_LOGW(TAG, "margin result delivery failed");
    }
}

static void SaveMargins(MarginNotice notice = MarginNotice::None) {
    if (s_dirty == 0) return;
    const uint8_t dirty = s_dirty;
    const uint32_t epoch = VkKey_InputEpoch();
    const int values[] = {DisplayUiOffset(0), DisplayUiOffset(1), DisplayUiOffset(2), DisplayUiOffset(3)};
    if (Application::GetInstance().Schedule([dirty, values, epoch, notice] {
        const char* keys[] = {"offset_top", "offset_bottom", "offset_left", "offset_right"};
        Settings settings("screen", true);
        bool success = true;
        for (int side = 0; side < kDisplayUiOffsetCount; ++side) {
            if ((dirty & (1U << side)) && !settings.SetInt(keys[side], values[side])) success = false;
        }
        success = settings.Commit() && success;
        PostSaveResult(epoch, notice, success);
    }, Application::TaskDomain::Storage)) {
        s_dirty = 0;
    } else {
        ESP_LOGE(TAG, "margin save scheduling failed: sides=0x%x", static_cast<unsigned>(dirty));
        if (notice != MarginNotice::None) ScreenShowToast(Lang::Strings::SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}

static void OnScreenEvent(lv_event_t* event) {
    auto* screen = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (s_screen == screen) SaveMargins();
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOAD_START) {
        if (auto* frame = EpdRenderBuffer()) {
            std::memset(frame, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
        }
    } else {
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        if (s_screen == screen) {
            s_screen = nullptr;
            s_background = nullptr;
            std::fill_n(s_values, kDisplayUiOffsetCount, nullptr);
            std::fill_n(s_sliders, kDisplayUiOffsetCount, nullptr);
        }
    }
}

static void RefreshValue(int side) {
    const int value = DisplayUiOffset(side);
    lv_label_set_text_fmt(s_values[side], Lang::Strings::SETTINGS_SCREEN_MARGIN_VALUE_FMT, value);
    lv_slider_set_value(s_sliders[side], value, LV_ANIM_OFF);
}

static void ApplyMargin(int side, int value) {
    if (!DisplayUiSetOffset(side, value)) return;
    s_dirty |= 1U << side;
    RefreshValue(side);
}

static void OnResetClicked(lv_event_t*) {
    if (DisplayUiResetOffsets()) {
        for (int side = 0; side < kDisplayUiOffsetCount; ++side) RefreshValue(side);
    }
    s_dirty = (1U << kDisplayUiOffsetCount) - 1;
    SaveMargins(MarginNotice::Reset);
}

static void OnSetDefaultsClicked(lv_event_t*) {
    SaveMargins();
    const uint32_t epoch = VkKey_InputEpoch();
    const int values[] = {DisplayUiOffset(0), DisplayUiOffset(1), DisplayUiOffset(2), DisplayUiOffset(3)};
    if (Application::GetInstance().Schedule([values, epoch] {
        const char* keys[] = {"off_def_top", "off_def_bottom", "off_def_left", "off_def_right"};
        Settings settings("screen", true);
        bool success = true;
        for (int side = 0; side < kDisplayUiOffsetCount; ++side) {
            if (!settings.SetInt(keys[side], values[side])) success = false;
        }
        success = settings.Commit() && success;
        PostSaveResult(epoch, MarginNotice::Set, success);
    }, Application::TaskDomain::Storage)) {
        DisplayUiSetOffsetDefaults();
    } else {
        ESP_LOGE(TAG, "margin defaults save scheduling failed");
        ScreenShowToast(Lang::Strings::SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}

static void OnStep(lv_event_t* event) {
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int side = action / 2;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        ApplyMargin(side, std::clamp<int>(DisplayUiOffset(side) + (action % 2 ? 1 : -1), kDisplayUiOffsetMin, kDisplayUiOffsetMax));
    } else {
        SaveMargins();
    }
}

static void OnSlider(lv_event_t* event) {
    const int side = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        ApplyMargin(side, lv_slider_get_value(s_sliders[side]));
    } else {
        SaveMargins();
    }
}

static lv_obj_t* Container(lv_obj_t* parent, lv_flex_flow_t flow) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, flow);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

void SettingsTestMarginScreen::SetStandby(bool standby) {
    if (s_screen == nullptr || s_screen != lv_screen_active() || s_standby == standby) return;
    s_standby = standby;
    if (standby) SaveMargins();
    if (standby) lv_obj_add_flag(s_background, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_background, LV_OBJ_FLAG_HIDDEN);
    if (auto* frame = EpdRenderBuffer()) {
        std::memset(frame, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
    }
    lv_obj_invalidate(s_screen);
}

static void BuildEdge(lv_obj_t* parent, int side, const char* name) {
    auto* edge = Container(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(edge, UiSy(8), 0);
    lv_obj_set_style_pad_row(edge, UiSy(6), 0);
    if (side != 3) {
        lv_obj_set_style_border_side(edge, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(edge, 1, 0);
        lv_obj_set_style_border_color(edge, lv_color_black(), 0);
    }
    auto* heading = Container(edge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* title = lv_label_create(heading);
    lv_label_set_text(title, name);
    s_values[side] = lv_label_create(heading);
    auto* row = Container(edge, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(row, kSettingsOptionH);
    lv_obj_set_style_pad_column(row, UiSx(12), 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int step = 0; step < 2; ++step) {
        auto* button = lv_button_create(row);
        auto* label = lv_label_create(button);
        lv_label_set_text(label, step == 0 ? Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC :
                                            Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_INC);
        SettingsStyleBrightnessButton(button);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(button);
        for (auto code : {LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(button, OnStep, code,
                reinterpret_cast<void*>(static_cast<intptr_t>(side * 2 + step)));
        }
        if (step != 0) continue;
        auto* slider = lv_slider_create(row);
        s_sliders[side] = slider;
        lv_obj_set_width(slider, 0);
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_set_height(slider, UiSy(16));
        lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
        lv_slider_set_range(slider, kDisplayUiOffsetMin, kDisplayUiOffsetMax);
        for (auto code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(slider, OnSlider, code, reinterpret_cast<void*>(static_cast<intptr_t>(side)));
        }
    }
    RefreshValue(side);
}

lv_obj_t* SettingsTestMarginScreen::Create() {
    if (auto* display = LvAdapterEpdiy::Instance()) {
        display->BindStatusBar(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    ScreenSetIsHome(false);
    auto* screen = lv_obj_create(nullptr);
    s_screen = screen;
    s_dirty = 0;
    s_standby = false;
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_background = lv_image_create(screen);
    lv_image_set_src(s_background, BackgroundImage());
    lv_obj_set_size(s_background, lv_pct(100), lv_pct(100));
    lv_image_set_inner_align(s_background, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(s_background, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_background, LV_OBJ_FLAG_SCROLLABLE);
    auto* controls = Container(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(controls, lv_pct(82));
    lv_obj_set_style_pad_row(controls, UiSy(6), 0);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(controls);
    auto* title = lv_label_create(controls);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_MARGIN);
    auto* note = lv_label_create(controls);
    lv_label_set_text(note, Lang::Strings::SETTINGS_SCREEN_MARGIN_TITLE);
    const char* names[] = {Lang::Strings::SETTINGS_SCREEN_MARGIN_TOP, Lang::Strings::SETTINGS_SCREEN_MARGIN_BOTTOM,
                          Lang::Strings::SETTINGS_SCREEN_MARGIN_LEFT, Lang::Strings::SETTINGS_SCREEN_MARGIN_RIGHT};
    for (int side = 0; side < kDisplayUiOffsetCount; ++side) BuildEdge(controls, side, names[side]);
    lv_obj_update_layout(controls);
    ScreenFitLabel(note, lv_obj_get_content_width(controls), fontpack_lv_font_ui()->line_height);
    auto* defaults = Container(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(defaults, lv_pct(82));
    lv_obj_align(defaults, LV_ALIGN_BOTTOM_MID, 0, -UiSy(16) - kSettingsOptionH - UiSy(12));
    lv_obj_set_style_pad_column(defaults, UiSx(12), 0);
    auto* reset = SettingsCreateSelectableOption(defaults, Lang::Strings::SETTINGS_SCREEN_MARGIN_RESET,
                                                OnResetClicked, 0);
    auto* save = SettingsCreateSelectableOption(defaults, Lang::Strings::SETTINGS_SCREEN_MARGIN_SET_DEFAULT,
                                               OnSetDefaultsClicked, 0);
    for (auto* button : {reset, save}) {
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
    }
    lv_obj_add_event_cb(screen, OnScreenEvent, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(screen, OnScreenEvent, LV_EVENT_DELETE, nullptr);
    VkKey_AttachScreen(screen, "settings_test_margin", {.factory = SettingsTestMarginScreen::Create});
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    ESP_LOGI(TAG, "open margin test");
    return screen;
}
