#pragma GCC optimize("O1")

#include "screen_frontlight.h"
#include "screen_common.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "standby_screen/standby_screen.h"
#include "cloud_screen/ap/ap_transfer.h"
#include "dual_network_board.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "quick_control_icons.h"
#include "haptic_feedback.h"
#include "lv_adapter_epdiy.h"
#include "power_policy.h"
#include "settings.h"
#include "ui_scale.h"

#include <algorithm>
#include <atomic>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>
#include <font_awesome.h>

LV_FONT_DECLARE(font_awesome_30_1);

#define TAG "QuickControls"

namespace {
constexpr int kDesignWidth = 743; // 参考布局宽度
constexpr int kDesignHeight = 327; // 参考布局高度
constexpr int kDefaultBrightness = 40; // 零亮度重新开启时使用的亮度

enum class WifiState {
    Unknown = -1, // 尚未绘制
    Switching, // 开关切换中
    Failed, // 开关操作失败
    Off, // Wi-Fi 已关闭或当前为蜂窝网络
    AccessPoint, // 热点已开启
    Connected, // 已连接无线网络
    Connecting, // 正在连接无线网络
    Disconnected, // 已开启但未连接
};

struct PanelUi {
    lv_obj_t* root = nullptr; // 全屏触摸遮罩
    EpdStatusBar previous_status; // 原页面状态栏绑定
    lv_obj_t* panel_status_label = nullptr; // 下拉状态栏绑定标识
    lv_obj_t* wifi = nullptr; // Wi-Fi 用户开关
    lv_obj_t* wifi_icon = nullptr; // Wi-Fi 图标
    lv_obj_t* wifi_title = nullptr; // Wi-Fi 标题
    lv_obj_t* wifi_status = nullptr; // 网络状态
    lv_obj_t* wifi_paw = nullptr; // Wi-Fi 右侧猫爪
    lv_obj_t* light_switch = nullptr; // 前光开关
    lv_obj_t* light_title = nullptr; // 前光标题
    lv_obj_t* light_status = nullptr; // 前光开关状态
    lv_obj_t* light_icon = nullptr; // 前光开关图标
    lv_obj_t* brightness = nullptr; // 亮度滑条
    lv_obj_t* warmth = nullptr; // 色温滑条
    lv_timer_t* timer = nullptr; // 网络状态刷新
    WifiState wifi_state = WifiState::Unknown; // 已绘制的网络状态
    int light_state = -1; // 已绘制的前光开关状态
};
PanelUi s_ui;
std::atomic<bool> s_wifi_busy{false};
std::atomic<bool> s_wifi_failed{false};
std::atomic<bool> s_save_running{false};
std::atomic<uint32_t> s_saved_value{0}; // 低三个字节依次保存亮度、暖光比例和通道组合
bool s_light_dirty = false; // LVGL 上下文持有，保存任务创建失败后跨面板保留

void SaveLightTask(void*) {
    for (;;) {
        const uint32_t value = s_saved_value.load();
        {
            Settings settings("frontlight", true);
            settings.SetInt("brightness", value & 0xff);
            settings.SetInt("warmth", (value >> 8) & 0xff);
            settings.SetInt("cct", (value >> 16) & 0xff);
        }
        s_save_running.store(false);
        if (value == s_saved_value.load() || s_save_running.exchange(true)) break;
    }
    vTaskDelete(nullptr);
}

void SaveLight() {
    if (!s_light_dirty) return;
    auto& light = Frontlight::GetInstance();
    s_saved_value.store(light.target_brightness() | (light.warmth() << 8) |
                        (static_cast<uint32_t>(light.cct()) << 16));
    if (!s_save_running.exchange(true) &&
        xTaskCreate(SaveLightTask, "quick_fl_save", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_save_running.store(false);
        ESP_LOGW(TAG, "frontlight save task failed");
        return;
    }
    s_light_dirty = false;
}

bool LightIsOn() {
    auto& light = Frontlight::GetInstance();
    return light.cct() != FrontlightCct::kOff && light.target_brightness() > 0;
}

void RefreshLightSwitch() {
    const bool on = LightIsOn();
    if (s_ui.light_state == static_cast<int>(on)) return;
    s_ui.light_state = static_cast<int>(on);
    lv_obj_set_style_bg_color(s_ui.light_switch, on ? lv_color_black() : lv_color_white(), 0);
    for (auto* label : {s_ui.light_title, s_ui.light_status})
        lv_obj_set_style_text_color(label, on ? lv_color_white() : lv_color_black(), 0);
    lv_label_set_text(s_ui.light_status, on ? "已开启" : "已关闭");
    for (auto* slider : {s_ui.brightness, s_ui.warmth})
        lv_obj_set_style_bg_color(slider, on ? lv_color_black() : lv_color_white(), LV_PART_INDICATOR);
    lv_image_set_src(s_ui.light_icon, on ? &kQuickBrightnessImage : &kQuickBrightnessMask);
    lv_obj_set_style_image_recolor(s_ui.light_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_ui.light_icon, on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

void LightChanged(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
        const auto value = static_cast<uint8_t>(lv_slider_get_value(slider));
        if (slider == s_ui.brightness) {
            light.SetBrightness(value, false);
            if (light.cct() == FrontlightCct::kOff) light.SetWarmth(light.warmth());
        } else {
            light.SetWarmth(value);
            if (light.target_brightness() == 0) light.SetBrightness(kDefaultBrightness, false);
            lv_slider_set_value(s_ui.brightness, light.target_brightness(), LV_ANIM_OFF);
        }
        s_light_dirty = true;
        RefreshLightSwitch();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SaveLight();
    }
}

void ToggleLight(lv_event_t*) {
    auto& light = Frontlight::GetInstance();
    if (LightIsOn()) {
        light.SetCct(FrontlightCct::kOff);
    } else {
        light.SetWarmth(light.warmth());
        if (light.target_brightness() == 0) light.SetBrightness(kDefaultBrightness, false);
    }
    lv_slider_set_value(s_ui.brightness, light.target_brightness(), LV_ANIM_OFF);
    s_light_dirty = true;
    RefreshLightSwitch();
    SaveLight();
}

bool WifiNetworkSelected() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual == nullptr || dual->GetNetworkType() == NetworkType::WIFI;
}

void RefreshWifi(lv_timer_t*) {
    if (!s_ui.root) return;
    const bool available = WifiNetworkSelected();
    auto* wifi = available ? &WifiStation::GetInstance() : nullptr;
    const bool enabled = wifi != nullptr && wifi->IsUserEnabled();
    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool ap = esp_wifi_get_mode(&mode) == ESP_OK && (mode & WIFI_MODE_AP) != 0;
    wifi_ap_record_t ap_info = {};
    const bool connected = enabled && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    WifiState state = WifiState::Off;
    if (available) {
        if (s_wifi_busy.load()) state = WifiState::Switching;
        else if (s_wifi_failed.load()) state = WifiState::Failed;
        else if (!enabled) state = WifiState::Off;
        else if (ap) state = WifiState::AccessPoint;
        else if (connected) state = WifiState::Connected;
        else if (wifi->IsLinkInProgress()) state = WifiState::Connecting;
        else state = WifiState::Disconnected;
    }
    // 网络状态不变时不改对象，避免周期性触发墨水屏刷新。
    if (state == s_ui.wifi_state) return;
    s_ui.wifi_state = state;
    static constexpr const char* status[] = {"切换中", "操作失败，请重试", "已关闭", "已开启", "已连接", "连接中", "未连接"};
    lv_label_set_text(s_ui.wifi_status, status[static_cast<int>(state)]);
    const auto foreground = enabled ? lv_color_white() : lv_color_black();
    lv_obj_set_style_bg_color(s_ui.wifi, enabled ? lv_color_black() : lv_color_white(), 0);
    for (auto* label : {s_ui.wifi_icon, s_ui.wifi_title, s_ui.wifi_status})
        lv_obj_set_style_text_color(label, foreground, 0);
    lv_obj_set_style_image_recolor(s_ui.wifi_paw, foreground, 0);
    lv_obj_set_style_image_recolor_opa(s_ui.wifi_paw, LV_OPA_COVER, 0);
    if (!available || s_wifi_busy.load()) lv_obj_add_state(s_ui.wifi, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_ui.wifi, LV_STATE_DISABLED);
}

void ToggleWifiTask(void* arg) {
    const bool enabled = arg != nullptr;
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        const esp_err_t result = WifiStation::GetInstance().SetUserEnabled(enabled);
        if (result == ESP_OK && !enabled) ApTransferStop(false);
        s_wifi_failed.store(result != ESP_OK);
        PowerPolicy::GetInstance().NotifyUserActivity();
        PowerPolicy::GetInstance().RequestReevaluate();
    }
    s_wifi_busy.store(false);
    vTaskDelete(nullptr);
}

void ToggleWifi(lv_event_t*) {
    if (!WifiNetworkSelected()) return;
    if (s_wifi_busy.exchange(true)) return;
    s_wifi_failed.store(false);
    const bool enabled = !WifiStation::GetInstance().IsUserEnabled();
    if (xTaskCreate(ToggleWifiTask, "quick_wifi", 4096,
                    enabled ? reinterpret_cast<void*>(1) : nullptr, 5, nullptr) != pdPASS) {
        s_wifi_failed.store(true);
        s_wifi_busy.store(false);
        ESP_LOGW(TAG, "Wi-Fi switch task failed");
    }
    RefreshWifi(nullptr);
}

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* NativeIcon(lv_obj_t* parent, const lv_image_dsc_t* source) {
    auto* image = lv_image_create(parent);
    lv_image_set_src(image, source);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_FLOATING);
    return image;
}

void DrawSliderIcon(lv_event_t* event) {
    auto* icon = static_cast<lv_obj_t*>(lv_event_get_target(event));
    auto* slider = lv_obj_get_parent(icon);
    auto* layer = lv_event_get_layer(event);
    lv_area_t icon_area, slider_area;
    lv_obj_get_coords(icon, &icon_area);
    lv_obj_get_coords(slider, &slider_area);
    const int fill_top = slider_area.y2 + 1 -
        lv_area_get_height(&slider_area) * lv_slider_get_value(slider) / 100;
    const auto clip = layer->_clip_area;
    const bool on = LightIsOn();
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = lv_event_get_user_data(event);
    dsc.recolor_opa = LV_OPA_COVER;
    // 两段裁剪共用原尺寸透明轮廓，边界与无内边距的竖向滑条填充一致。
    for (bool filled : {false, true}) {
        lv_area_t part = icon_area;
        if (on) {
            if (filled) part.y1 = std::max<int>(part.y1, fill_top);
            else part.y2 = std::min<int>(part.y2, fill_top - 1);
        } else if (filled) continue;
        part.x1 = std::max(part.x1, clip.x1);
        part.x2 = std::min(part.x2, clip.x2);
        part.y1 = std::max(part.y1, clip.y1);
        part.y2 = std::min(part.y2, clip.y2);
        if (part.x1 > part.x2 || part.y1 > part.y2) continue;
        layer->_clip_area = part;
        dsc.recolor = filled ? lv_color_white() : lv_color_black();
        lv_draw_image(layer, &dsc, &icon_area);
    }
    layer->_clip_area = clip;
}

lv_obj_t* Slider(lv_obj_t* parent, int x, int y, int width, int height, int value, bool warmth) {
    auto* slider = lv_slider_create(parent);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, width, height);
    lv_slider_set_orientation(slider, LV_SLIDER_ORIENTATION_VERTICAL);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 2, LV_PART_MAIN);
    lv_obj_set_style_border_post(slider, true, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(slider, width / 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    for (auto code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(slider, LightChanged, code, nullptr);
    const auto* mask = warmth ? &kQuickColorTempMask : &kQuickBrightnessMask;
    auto* icon = Box(slider, 0, 0, mask->header.w, mask->header.h);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(icon, DrawSliderIcon, LV_EVENT_DRAW_MAIN,
                        const_cast<lv_image_dsc_t*>(mask));
    lv_obj_align(icon, LV_ALIGN_BOTTOM_MID, 0, -UiSy(8));
    return slider;
}
} // namespace

bool ScreenFrontlightIsVisible() {
    return s_ui.root != nullptr;
}

bool ScreenFrontlightOwnsDrag(int x, int y) {
    for (auto* slider : {s_ui.brightness, s_ui.warmth}) {
        if (!slider) continue;
        lv_area_t area;
        lv_obj_get_coords(slider, &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) return true;
    }
    return false;
}

void ScreenFrontlightHide() {
    if (!s_ui.root) return;
    SaveLight();
    if (auto* display = LvAdapterEpdiy::Instance()) display->RequestFullOnNextCommit();
    lv_obj_delete(s_ui.root);
}

void ScreenFrontlightShow() {
    if (OtaUpgradeScreen::IsActive() || StandbyScreen::IsActive() ||
        s_ui.root || !Frontlight::GetInstance().initialized()) return;
    auto* root = Box(lv_layer_top(), 0, 0, LV_HOR_RES, LV_VER_RES);
    s_ui.root = root;
    lv_obj_add_event_cb(root, [](lv_event_t*) {
        if (s_ui.timer) lv_timer_delete(s_ui.timer);
        if (auto* display = LvAdapterEpdiy::Instance(); display &&
            display->GetBoundStatusBar().status_label == s_ui.panel_status_label) {
            auto status = s_ui.previous_status;
            if (status.status_label && !lv_obj_is_valid(status.status_label)) status = {};
            display->BindStatusBar(status.status_label, status.notification_label, status.network_label,
                                   status.battery_label, status.battery_pct_label, status.mute_label);
        }
        s_ui = {};
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(root, [](lv_event_t*) { ScreenFrontlightHide(); }, LV_EVENT_CLICKED, nullptr);
    if (auto* display = LvAdapterEpdiy::Instance()) s_ui.previous_status = display->GetBoundStatusBar();
    const auto status = ScreenCreateStatusBar(root);
    s_ui.panel_status_label = status.status_label;
    lv_obj_set_style_bg_color(status.bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status.bar, LV_OPA_COVER, 0);
    if (auto* display = LvAdapterEpdiy::Instance()) display->UpdateStatusBar(true);
    const int width = LV_HOR_RES;
    const int panel_y = ScreenStatusBarHeight();
    const int height = std::min<int>(LV_VER_RES - panel_y, width * kDesignHeight / kDesignWidth);
    const int radius = width * 28 / kDesignWidth;
    auto* clip = Box(root, 0, panel_y, width, height);
    // 将顶部圆角移出矩形裁剪区，保留底部圆角和连续的两侧边框。
    auto* panel = Box(clip, 0, -radius, width, height + radius);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_side(panel, static_cast<lv_border_side_t>(
        LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    const int top = radius + height * 40 / kDesignHeight;
    const int slider_h = height * 249 / kDesignHeight;
    const int slider_w = width * 122 / kDesignWidth;
    const int wifi_h = height * 107 / kDesignHeight;
    const auto* status_font = fontpack_lv_font_get(25, 2);
    s_ui.wifi = Box(panel, width * 58 / kDesignWidth, top, width * 283 / kDesignWidth, wifi_h);
    lv_obj_set_style_bg_opa(s_ui.wifi, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.wifi, 2, 0);
    lv_obj_set_style_border_color(s_ui.wifi, lv_color_black(), 0);
    lv_obj_set_style_radius(s_ui.wifi, wifi_h * 2 / 5, 0);
    HapticAttachClick(s_ui.wifi);
    lv_obj_add_event_cb(s_ui.wifi, ToggleWifi, LV_EVENT_CLICKED, nullptr);
    s_ui.wifi_icon = Label(s_ui.wifi, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(s_ui.wifi_icon, &font_awesome_30_1, 0);
    lv_obj_set_style_transform_scale(s_ui.wifi_icon, 384, 0);
    lv_obj_align(s_ui.wifi_icon, LV_ALIGN_LEFT_MID, width * 25 / kDesignWidth, -width * 8 / kDesignWidth);
    s_ui.wifi_title = Label(s_ui.wifi, "Wi-Fi");
    lv_obj_set_pos(s_ui.wifi_title, width * 100 / kDesignWidth, wifi_h / 5);
    s_ui.wifi_status = Label(s_ui.wifi, "");
    lv_obj_set_width(s_ui.wifi_status, width * 283 / kDesignWidth - kQuickCatPawImage.header.w + 15 - width * 100 / kDesignWidth - 8);
    lv_obj_set_style_text_font(s_ui.wifi_status, status_font, 0);
    lv_obj_set_height(s_ui.wifi_status, status_font->line_height);
    lv_label_set_long_mode(s_ui.wifi_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ui.wifi_status, width * 100 / kDesignWidth, wifi_h * 3 / 5);
    s_ui.wifi_paw = NativeIcon(s_ui.wifi, &kQuickCatPawImage);
    lv_obj_align(s_ui.wifi_paw, LV_ALIGN_RIGHT_MID, -4, 0);
    s_ui.light_switch = Box(panel, width * 58 / kDesignWidth, top + slider_h - wifi_h, width * 283 / kDesignWidth, wifi_h);
    lv_obj_set_style_bg_opa(s_ui.light_switch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.light_switch, 2, 0);
    lv_obj_set_style_border_color(s_ui.light_switch, lv_color_black(), 0);
    lv_obj_set_style_radius(s_ui.light_switch, wifi_h * 2 / 5, 0);
    HapticAttachClick(s_ui.light_switch);
    lv_obj_add_event_cb(s_ui.light_switch, ToggleLight, LV_EVENT_CLICKED, nullptr);
    s_ui.light_icon = NativeIcon(s_ui.light_switch, &kQuickBrightnessImage);
    lv_obj_align(s_ui.light_icon, LV_ALIGN_LEFT_MID, 6, -16);
    s_ui.light_title = Label(s_ui.light_switch, "前光");
    lv_obj_set_pos(s_ui.light_title, width * 100 / kDesignWidth, wifi_h / 5);
    s_ui.light_status = Label(s_ui.light_switch, "");
    lv_obj_set_style_text_font(s_ui.light_status, status_font, 0);
    lv_obj_set_pos(s_ui.light_status, width * 100 / kDesignWidth, wifi_h * 3 / 5);
    auto& light = Frontlight::GetInstance();
    s_ui.brightness = Slider(panel, width * 383 / kDesignWidth, top, slider_w, slider_h,
                             light.target_brightness(), false);
    s_ui.warmth = Slider(panel, width * 545 / kDesignWidth, top, slider_w, slider_h, light.warmth(), true);
    RefreshLightSwitch();
    RefreshWifi(nullptr);
    s_ui.timer = lv_timer_create(RefreshWifi, 1000, nullptr);
    if (auto* display = LvAdapterEpdiy::Instance()) display->RequestFullOnNextCommit();
}
