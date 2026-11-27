#pragma GCC optimize("O1")

#include <algorithm>
#include <esp_log.h>
#include <esp_wifi.h>
#include <wifi_station.h>
#include <font_awesome.h>
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/display/frontlight.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "business/network/network_settings.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/quick_control_icons.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

LV_FONT_DECLARE(font_awesome_30_1);

#define TAG "QuickControls"

static void (*s_changed)() = nullptr;

static UiModal s_window_modal;

void ScreenFrontlightSetChangedCallback(void (*changed)()) {
    s_changed = changed;
}

static constexpr int kDesignWidth = 743;  // 参考布局宽度

enum class ScreenFrontlightWifiState {
    Unknown = -1,  // 尚未绘制
    Switching,     // 开关切换中
    Failed,        // 开关操作失败
    Off,           // Wi-Fi 已关闭或当前为蜂窝网络
    AccessPoint,   // 热点已开启
    Connected,     // 已连接无线网络
    Connecting,    // 正在连接无线网络
    Disconnected,  // 已开启但未连接
};

struct ScreenFrontlightPanelUi {
    lv_obj_t* root = nullptr;                                                   // 全屏触摸遮罩
    EpdStatusBar previous_status;                                               // 原页面状态栏绑定
    lv_obj_t* panel_status_label = nullptr;                                     // 下拉状态栏绑定标识
    lv_obj_t* wifi = nullptr;                                                   // Wi-Fi 用户开关
    lv_obj_t* wifi_icon = nullptr;                                              // Wi-Fi 图标
    lv_obj_t* wifi_title = nullptr;                                             // Wi-Fi 标题
    lv_obj_t* wifi_status = nullptr;                                            // 网络状态
    lv_obj_t* wifi_paw = nullptr;                                               // Wi-Fi 右侧猫爪
    lv_obj_t* light_switch = nullptr;                                           // 前光开关
    lv_obj_t* light_title = nullptr;                                            // 前光标题
    lv_obj_t* light_status = nullptr;                                           // 前光开关状态
    lv_obj_t* light_icon = nullptr;                                             // 前光开关图标
    lv_obj_t* brightness = nullptr;                                             // 亮度滑条
    lv_obj_t* warmth = nullptr;                                                 // 色温滑条
    lv_timer_t* timer = nullptr;                                                // 网络状态刷新
    ScreenFrontlightWifiState wifi_state = ScreenFrontlightWifiState::Unknown;  // 已绘制的网络状态
    int light_state = -1;                                                       // 已绘制的前光开关状态
};

static ScreenFrontlightPanelUi s_ui;
static bool s_light_dirty = false;  // 保存请求失败时保留待保存状态

static void SaveLight() {
    if (s_light_dirty && Frontlight::GetInstance().SavePreferences()) {
        s_light_dirty = false;
    }
}

static bool LightIsOn() {
    auto& light = Frontlight::GetInstance();
    return light.cct() != FrontlightCct::kOff && light.target_brightness() > 0;
}

static void RefreshLightSwitch() {
    const bool on = LightIsOn();
    if (s_ui.light_state == static_cast<int>(on)) {
        return;
    }
    s_ui.light_state = static_cast<int>(on);
    lv_obj_set_style_bg_color(s_ui.light_switch, on ? UiColorForeground() : UiColorBackground(), 0);
    for (auto* label : {s_ui.light_title, s_ui.light_status}) {
        lv_obj_set_style_text_color(label, on ? UiColorBackground() : UiColorForeground(), 0);
    }
    lv_label_set_text(s_ui.light_status, on ? LangString_QUICK_ENABLED : LangString_QUICK_DISABLED);
    for (auto* slider : {s_ui.brightness, s_ui.warmth}) {
        lv_obj_set_style_bg_color(slider, on ? UiColorForeground() : UiColorBackground(), LV_PART_INDICATOR);
    }
    const bool mask = !on || UiThemeIsDark();
    lv_image_set_src(s_ui.light_icon, mask ? &kQuickBrightnessMask.image : &kQuickBrightnessImage.image);
    lv_obj_set_style_image_recolor(s_ui.light_icon, on ? UiColorBackground() : UiColorForeground(), 0);
    lv_obj_set_style_image_recolor_opa(s_ui.light_icon, mask ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void LightChanged(lv_event_t* event) {
    auto& light = Frontlight::GetInstance();
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
        const auto value = static_cast<uint8_t>(lv_slider_get_value(slider));
        if (slider == s_ui.brightness) {
            light.SetBrightness(value, false);
            if (light.cct() == FrontlightCct::kOff) {
                light.SetWarmth(light.warmth());
            }
        } else {
            light.SetWarmth(value);
            if (light.target_brightness() == 0) {
                light.SetBrightness(kFrontlightDefaultBrightness, false);
            }
            lv_slider_set_value(s_ui.brightness, light.target_brightness(), LV_ANIM_OFF);
        }
        s_light_dirty = true;
        RefreshLightSwitch();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SaveLight();
    }
}

static void ToggleLight(lv_event_t*) {
    auto& light = Frontlight::GetInstance();
    light.SetEnabled(!LightIsOn());
    lv_slider_set_value(s_ui.brightness, light.target_brightness(), LV_ANIM_OFF);
    s_light_dirty = true;
    RefreshLightSwitch();
    SaveLight();
}

static bool WifiNetworkSelected() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual == nullptr || dual->GetNetworkType() == NetworkType::WIFI;
}

static void RefreshWifi(lv_timer_t*) {
    if (!s_ui.root) {
        return;
    }
    const bool available = WifiNetworkSelected();
    auto* wifi = available ? &WifiStation::GetInstance() : nullptr;
    const bool enabled = wifi != nullptr && wifi->IsUserEnabled();
    const auto switch_state = NetworkWifiGetSwitchState();
    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool ap = esp_wifi_get_mode(&mode) == ESP_OK && (mode & WIFI_MODE_AP) != 0;
    wifi_ap_record_t ap_info = {};
    const bool connected = enabled && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    ScreenFrontlightWifiState state = ScreenFrontlightWifiState::Off;
    if (available) {
        if (switch_state == NetworkWifiSwitchState::kSwitching) {
            state = ScreenFrontlightWifiState::Switching;
        } else if (switch_state == NetworkWifiSwitchState::kFailed) {
            state = ScreenFrontlightWifiState::Failed;
        } else if (!enabled) {
            state = ScreenFrontlightWifiState::Off;
        } else if (ap) {
            state = ScreenFrontlightWifiState::AccessPoint;
        } else if (connected) {
            state = ScreenFrontlightWifiState::Connected;
        } else if (wifi->IsLinkInProgress()) {
            state = ScreenFrontlightWifiState::Connecting;
        } else {
            state = ScreenFrontlightWifiState::Disconnected;
        }
    }
    // 网络状态不变时不改对象，避免周期性触发墨水屏刷新。
    if (state == s_ui.wifi_state) {
        return;
    }
    s_ui.wifi_state = state;
    const char* status[] = {LangString_QUICK_SWITCHING,   LangString_QUICK_FAILED,    LangString_QUICK_DISABLED,
                            LangString_QUICK_ENABLED,     LangString_QUICK_CONNECTED, LangString_QUICK_CONNECTING,
                            LangString_QUICK_DISCONNECTED};
    lv_label_set_text(s_ui.wifi_status, status[static_cast<int>(state)]);
    lv_obj_update_layout(s_ui.wifi);
    ScreenFitLabel(s_ui.wifi_status, lv_obj_get_width(s_ui.wifi_status), lv_obj_get_height(s_ui.wifi) * 2 / 5 - 4,
                   UiFont(UiFontRole::Caption));
    lv_obj_set_style_text_align(s_ui.wifi_status, LV_TEXT_ALIGN_LEFT, 0);
    const auto foreground = enabled ? UiColorBackground() : UiColorForeground();
    lv_obj_set_style_bg_color(s_ui.wifi, enabled ? UiColorForeground() : UiColorBackground(), 0);
    for (auto* label : {s_ui.wifi_icon, s_ui.wifi_title, s_ui.wifi_status}) {
        lv_obj_set_style_text_color(label, foreground, 0);
    }
    lv_obj_set_style_image_recolor(s_ui.wifi_paw, foreground, 0);
    lv_obj_set_style_image_recolor_opa(s_ui.wifi_paw, LV_OPA_COVER, 0);
    if (!available || switch_state == NetworkWifiSwitchState::kSwitching) {
        lv_obj_add_state(s_ui.wifi, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_ui.wifi, LV_STATE_DISABLED);
    }
}

static void ToggleWifi(lv_event_t*) {
    NetworkWifiToggle();
    RefreshWifi(nullptr);
}

static lv_obj_t* Box(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t* Label(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t* NativeIcon(lv_obj_t* parent, const lv_image_dsc_t* source) {
    auto* image = lv_image_create(parent);
    lv_image_set_src(image, source);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_FLOATING);
    return image;
}

static void DrawSliderIcon(lv_event_t* event) {
    auto* icon = static_cast<lv_obj_t*>(lv_event_get_target(event));
    auto* slider = lv_obj_get_parent(icon);
    auto* layer = lv_event_get_layer(event);
    lv_area_t icon_area, slider_area;
    lv_obj_get_coords(icon, &icon_area);
    lv_obj_get_coords(slider, &slider_area);
    const int fill_top = slider_area.y2 + 1 - lv_area_get_height(&slider_area) * lv_slider_get_value(slider) / 100;
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
            if (filled) {
                part.y1 = std::max<int>(part.y1, fill_top);
            } else {
                part.y2 = std::min<int>(part.y2, fill_top - 1);
            }
        } else if (filled) {
            continue;
        }
        part.x1 = std::max(part.x1, clip.x1);
        part.x2 = std::min(part.x2, clip.x2);
        part.y1 = std::max(part.y1, clip.y1);
        part.y2 = std::min(part.y2, clip.y2);
        if (part.x1 > part.x2 || part.y1 > part.y2) {
            continue;
        }
        layer->_clip_area = part;
        dsc.recolor = filled ? UiColorBackground() : UiColorForeground();
        lv_draw_image(layer, &dsc, &icon_area);
    }
    layer->_clip_area = clip;
}

static lv_obj_t* Slider(lv_obj_t* parent, int x, int y, int width, int height, int value, bool warmth) {
    auto* slider = lv_slider_create(parent);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, width, height);
    lv_slider_set_orientation(slider, LV_SLIDER_ORIENTATION_VERTICAL);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UiColorBackground(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 2, LV_PART_MAIN);
    lv_obj_set_style_border_post(slider, true, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, UiColorForeground(), LV_PART_MAIN);
    lv_obj_set_style_radius(slider, width / 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UiColorForeground(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    for (auto code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(slider, LightChanged, code, nullptr);
    }
    const auto* mask = warmth ? &kQuickColorTempMask.image : &kQuickBrightnessMask.image;
    auto* icon = Box(slider, 0, 0, mask->header.w, mask->header.h);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(icon, DrawSliderIcon, LV_EVENT_DRAW_MAIN, const_cast<lv_image_dsc_t*>(mask));
    lv_obj_align(icon, LV_ALIGN_BOTTOM_MID, 0, -UiSy(8));
    return slider;
}

void ScreenFrontlightShow() {
    if (!HomeIconDecoderReady()) {
        return;
    }
    if (UiInputQuickPanelBlocked() || s_ui.root || (BOARD_HAS_FRONTLIGHT && !Frontlight::GetInstance().initialized())) {
        return;
    }
    auto* root = Box(lv_layer_top(), 0, 0, LV_HOR_RES, LV_VER_RES);
    s_ui.root = root;
    s_window_modal.Bind(root, [](void*) { ScreenFrontlightHide(); });
    lv_obj_add_event_cb(
        root,
        [](lv_event_t*) {
            if (s_ui.timer) {
                lv_timer_delete(s_ui.timer);
            }
            if (auto* display = LVAdapterDisplay::Instance();
                display && display->GetBoundStatusBar().status_label == s_ui.panel_status_label) {
                auto status = s_ui.previous_status;
                if (status.status_label && !lv_obj_is_valid(status.status_label)) {
                    status = {};
                }
                display->BindStatusBar(status.status_label, status.notification_label, status.network_label,
                                       status.battery_label, status.battery_pct_label, status.mute_label);
            }
            s_ui = {};
        },
        LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(root, [](lv_event_t*) { ScreenFrontlightHide(); }, LV_EVENT_CLICKED, nullptr);
    if (auto* display = LVAdapterDisplay::Instance()) {
        s_ui.previous_status = display->GetBoundStatusBar();
    }
    const auto status = ScreenCreateStatusBar(root);
    s_ui.panel_status_label = status.status_label;
    lv_obj_set_style_bg_color(status.bar, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(status.bar, LV_OPA_COVER, 0);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->UpdateStatusBar(true);
    }
    const int width = LV_HOR_RES;
    const int panel_y = ScreenStatusBarHeight();
    const auto* status_font = UiFont(UiFontRole::Caption);
    if (!status_font) {
        status_font = UiFont();
    }
    const int margin = width * 40 / kDesignWidth;
    const int inset = std::max(6, width * 12 / kDesignWidth);
    const int wifi_h = std::max<int>(width * 107 / kDesignWidth, 2 * status_font->line_height + 2 * inset + 4);
    const int slider_h = std::max(width * 249 / kDesignWidth, 2 * wifi_h + inset);
    const int content_h = BOARD_HAS_FRONTLIGHT ? slider_h : wifi_h;
    const int height = std::min<int>(LV_VER_RES - panel_y, content_h + 2 * margin);
    const int radius = width * 28 / kDesignWidth;
    auto* clip = Box(root, 0, panel_y, width, height);
    // 将顶部圆角移出矩形裁剪区，保留底部圆角和连续的两侧边框。
    auto* panel = Box(clip, 0, -radius, width, height + radius);
    lv_obj_set_style_bg_color(panel, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UiColorForeground(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_side(
        panel, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    const int top = radius + margin;
    const int slider_w = width * 122 / kDesignWidth;
    const int switch_w = BOARD_HAS_FRONTLIGHT ? width * 310 / kDesignWidth : width - 2 * margin;
    const int switch_x = BOARD_HAS_FRONTLIGHT ? width * 58 / kDesignWidth : margin;
    const int text_x = width * 100 / kDesignWidth;
    const int paw_edge = width * 80 / kDesignWidth;
    const int text_w = switch_w - text_x - paw_edge - inset;
    s_ui.wifi = Box(panel, switch_x, top, switch_w, wifi_h);
    lv_obj_set_style_bg_opa(s_ui.wifi, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.wifi, 2, 0);
    lv_obj_set_style_border_color(s_ui.wifi, UiColorForeground(), 0);
    lv_obj_set_style_radius(s_ui.wifi, wifi_h * 2 / 5, 0);
    HapticAttachClick(s_ui.wifi);
    lv_obj_add_event_cb(s_ui.wifi, ToggleWifi, LV_EVENT_CLICKED, nullptr);
    s_ui.wifi_icon = Label(s_ui.wifi, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(s_ui.wifi_icon, &font_awesome_30_1, 0);
    lv_obj_set_style_transform_scale(s_ui.wifi_icon, width * 384 / kDesignWidth, 0);
    lv_obj_align(s_ui.wifi_icon, LV_ALIGN_LEFT_MID, width * 25 / kDesignWidth, -width * 8 / kDesignWidth);
    s_ui.wifi_title = Label(s_ui.wifi, "Wi-Fi");
    lv_obj_set_style_text_font(s_ui.wifi_title, status_font, 0);
    lv_obj_set_pos(s_ui.wifi_title, text_x, wifi_h / 5);
    lv_obj_set_width(s_ui.wifi_title, text_w);
    lv_label_set_long_mode(s_ui.wifi_title, LV_LABEL_LONG_DOT);
    s_ui.wifi_status = Label(s_ui.wifi, "");
    lv_obj_set_pos(s_ui.wifi_status, text_x, wifi_h * 3 / 5);
    lv_obj_set_width(s_ui.wifi_status, text_w);
    lv_obj_set_style_text_font(s_ui.wifi_status, status_font, 0);
    lv_obj_set_height(s_ui.wifi_status, status_font->line_height);
    lv_label_set_long_mode(s_ui.wifi_status, LV_LABEL_LONG_DOT);
    s_ui.wifi_paw = NativeIcon(s_ui.wifi, &kQuickCatPawImage.image);
    lv_obj_set_size(s_ui.wifi_paw, paw_edge, paw_edge);
    lv_image_set_inner_align(s_ui.wifi_paw, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(s_ui.wifi_paw, LV_ALIGN_RIGHT_MID, -width * 4 / kDesignWidth, 0);
    if (BOARD_HAS_FRONTLIGHT) {
        s_ui.light_switch = Box(panel, switch_x, top + slider_h - wifi_h, switch_w, wifi_h);
        lv_obj_set_style_bg_opa(s_ui.light_switch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ui.light_switch, 2, 0);
        lv_obj_set_style_border_color(s_ui.light_switch, UiColorForeground(), 0);
        lv_obj_set_style_radius(s_ui.light_switch, wifi_h * 2 / 5, 0);
        HapticAttachClick(s_ui.light_switch);
        lv_obj_add_event_cb(s_ui.light_switch, ToggleLight, LV_EVENT_CLICKED, nullptr);
        s_ui.light_icon = NativeIcon(s_ui.light_switch, &kQuickBrightnessImage.image);
        lv_obj_align(s_ui.light_icon, LV_ALIGN_LEFT_MID, 6, -16);
        s_ui.light_title = Label(s_ui.light_switch, LangString_SETTINGS_FRONTLIGHT_TITLE);
        lv_obj_set_pos(s_ui.light_title, text_x, wifi_h / 5);
        s_ui.light_status = Label(s_ui.light_switch, "");
        lv_obj_set_style_text_font(s_ui.light_status, status_font, 0);
        lv_obj_set_pos(s_ui.light_status, text_x, wifi_h * 3 / 5);
        auto& light = Frontlight::GetInstance();
        s_ui.brightness =
            Slider(panel, width * 383 / kDesignWidth, top, slider_w, slider_h, light.target_brightness(), false);
        s_ui.warmth = Slider(panel, width * 545 / kDesignWidth, top, slider_w, slider_h, light.warmth(), true);
        RefreshLightSwitch();
    }
    RefreshWifi(nullptr);
    s_ui.timer = lv_timer_create(RefreshWifi, 1000, nullptr);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestFullOnNextCommit();
    }
}

void ScreenFrontlightHide() {
    if (!s_ui.root) {
        return;
    }
    SaveLight();
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestFullOnNextCommit();
    }
    lv_obj_delete(s_ui.root);
    if (s_changed != nullptr) {
        s_changed();
    }
}

bool ScreenFrontlightIsVisible() {
    return s_ui.root != nullptr;
}

bool ScreenFrontlightOwnsDrag(int x, int y) {
    for (auto* slider : {s_ui.brightness, s_ui.warmth}) {
        if (!slider) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(slider, &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return true;
        }
    }
    return false;
}
