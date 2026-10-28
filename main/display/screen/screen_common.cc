#include "screen_common.h"
#include "screen_frontlight.h"
#include "screen_common_ui.h"
#include "screen_common_nav.h"
#include "screen_common_priv.h"

#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "epd_refresh.h"
#include "haptic_feedback.h"
#include "lv_adapter_epdiy.h"
#include "network_screen/network_screen.h"
#include "settings_screen/settings_network_tab.h"
#include "vk_key_handler.h"

#include <cstring>

#include <esp_log.h>
#include <font_awesome.h>

LV_FONT_DECLARE(font_awesome_30_1);

bool ScreenCommon_on_home = true;
ScreenFactory ScreenCommon_back_stack[kMaxBack] = {};
int ScreenCommon_back_depth = 0;

lv_coord_t ScreenStatusBarHeight() {
    const lv_font_t* font = fontpack_lv_font_ui();
    return (font != nullptr ? font->line_height : 30) + kStatusPadV * 2;
}

namespace {
lv_timer_t* navigation_retry = nullptr;
lv_obj_t* navigation_origin = nullptr;
void (*pending_navigation)(void*) = nullptr;

// Keep navigation on the LVGL thread, without rebuilding a screen while the
// two LCD feeder tasks are still publishing the previous waveform's rows.
bool DeferNavigationWhileRefreshing(void (*navigate)(void*)) {
    if (!EpdGetStats().busy && pending_navigation == nullptr) return false;
    if (navigation_retry == nullptr) {
        navigation_retry = lv_timer_create([](lv_timer_t* timer) {
            if (EpdGetStats().busy) return;
            auto callback = pending_navigation;
            auto* origin = navigation_origin;
            pending_navigation = nullptr;
            navigation_origin = nullptr;
            lv_timer_pause(timer);
            // A different navigation may have completed during the wait.
            if (callback != nullptr && origin == lv_screen_active()) callback(nullptr);
        }, 20, nullptr);
        if (navigation_retry == nullptr) {
            ESP_LOGW(TAG, "cannot defer navigation while refreshing");
            return true;
        }
    }
    // Repeated back presses during one waveform produce one navigation.
    if (pending_navigation != ScreenCommon_GoHomeAsync) pending_navigation = navigate;
    navigation_origin = lv_screen_active();
    lv_timer_resume(navigation_retry);
    return true;
}
} // namespace

LVAdapterDisplay* ScreenCommon_GetAdapterDisplay() {
    return LVAdapterDisplay::Instance();
}

void ScreenCommon_OnStatusNetworkIconClicked(lv_event_t* /*e*/) {
    if (!ScreenIsHome()) {
        return;
    }
    if (!SettingsNetworkTab_IsWifi()) {
        return;
    }
    if (Board::GetInstance().IsNetworkReady()) {
        return;
    }
    ESP_LOGI(TAG, "home wifi icon -> NetworkScreen (wifi mode, not connected)");
    ScreenNavigateTo(NetworkScreen::Create);
}

void ScreenCommon_ClearBackStack() {
    ScreenCommon_back_depth = 0;
    for (int i = 0; i < kMaxBack; ++i) {
        ScreenCommon_back_stack[i] = nullptr;
    }
}

void ScreenCommon_PushBackFactory(ScreenFactory factory) {
    if (factory == nullptr) {
        return;
    }
    if (ScreenCommon_back_depth < kMaxBack) {
        ScreenCommon_back_stack[ScreenCommon_back_depth++] = factory;
    } else {
        for (int i = 1; i < kMaxBack; ++i) {
            ScreenCommon_back_stack[i - 1] = ScreenCommon_back_stack[i];
        }
        ScreenCommon_back_stack[kMaxBack - 1] = factory;
        ESP_LOGW(TAG, "back stack full, drop oldest");
    }
}

ScreenFactory ScreenCommon_PopBackFactory() {
    if (ScreenCommon_back_depth <= 0) {
        return nullptr;
    }
    --ScreenCommon_back_depth;
    ScreenFactory f = ScreenCommon_back_stack[ScreenCommon_back_depth];
    ScreenCommon_back_stack[ScreenCommon_back_depth] = nullptr;
    return f;
}

void ScreenCommon_GoHomeAsync(void* /*user_data*/) {
    if (ScreenFrontlightIsVisible()) {
        ScreenFrontlightHide();
        return;
    }
    if (ScreenCommon_on_home) {
        ESP_LOGI(TAG, "home ignored (already home)");
        return;
    }
    if (DeferNavigationWhileRefreshing(ScreenCommon_GoHomeAsync)) return;
    ESP_LOGI(TAG, "-> home (clear back stack depth was %d)", ScreenCommon_back_depth);
    ScreenGoHome();
}

void ScreenCommon_NavigateBackAsync(void* /*user_data*/) {
    if (DeferNavigationWhileRefreshing(ScreenCommon_NavigateBackAsync)) return;
    ScreenFactory prev = ScreenCommon_PopBackFactory();
    if (prev != nullptr) {
        ESP_LOGI(TAG, "navigate back -> recreate (stack left=%d)", ScreenCommon_back_depth);
        ScreenCommon_on_home = false;
        ScreenLoadReplace(prev());
        return;
    }
    ESP_LOGI(TAG, "navigate back -> home (empty stack)");
    ScreenGoHome();
}

void ScreenShowToast(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    static lv_obj_t* notice = nullptr;
    if (notice != nullptr) {
        lv_obj_delete(notice);
    }
    notice = lv_label_create(lv_layer_top());
    lv_obj_remove_style_all(notice);
    lv_label_set_text(notice, text);
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(notice, LV_HOR_RES - 32, 0);
    lv_obj_set_style_text_font(notice, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(notice, lv_color_white(), 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notice, 8, 0);
    lv_obj_set_style_pad_hor(notice, 12, 0);
    lv_obj_set_style_pad_ver(notice, 8, 0);
    lv_obj_clear_flag(notice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, ScreenStatusBarHeight() + 8);
    lv_obj_add_event_cb(notice, [](lv_event_t*) { notice = nullptr; },
                        LV_EVENT_DELETE, nullptr);
    lv_obj_delete_delayed(notice, 3000);
}

EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr) {
    EpdStatusBar out;

    const lv_font_t* ui_font = fontpack_lv_font_ui();
    out.height = ScreenStatusBarHeight();
    const lv_coord_t status_line_h = out.height - kStatusPadV * 2;

    out.bar = lv_obj_create(scr);
    lv_obj_set_size(out.bar, LV_HOR_RES, out.height);
    lv_obj_set_style_radius(out.bar, 0, 0);
    lv_obj_set_style_bg_opa(out.bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(out.bar, 0, 0);
    lv_obj_set_style_pad_all(out.bar, 0, 0);
    lv_obj_set_style_pad_top(out.bar, kStatusPadV, 0);
    lv_obj_set_style_pad_bottom(out.bar, kStatusPadV, 0);
    lv_obj_set_style_pad_left(out.bar, kStatusPadH, 0);
    lv_obj_set_style_pad_right(out.bar, kStatusPadH, 0);
    lv_obj_set_flex_flow(out.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(out.bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(out.bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(out.bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(out.bar, LV_OBJ_FLAG_CLICKABLE);

    // 底边横线挂 bar 下（FLOATING 不进 flex）；藏 bar 时一并隐藏。左右让出 pad。
    constexpr lv_coord_t kStatusRuleH = 2;
    lv_obj_t* rule = lv_obj_create(out.bar);
    lv_obj_remove_style_all(rule);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(rule, LV_HOR_RES - kStatusPadH * 2, kStatusRuleH);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    // content 坐标贴外底：越过 pad_bottom
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, kStatusPadV);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* net_hit = lv_obj_create(out.bar);
    lv_obj_remove_style_all(net_hit);
    lv_obj_set_size(net_hit, kStatusNetHitW, out.height - kStatusPadV * 2);
    if (lv_obj_get_height(net_hit) < kStatusNetHitMinH) {
        lv_obj_set_height(net_hit, kStatusNetHitMinH);
    }
    lv_obj_set_style_bg_opa(net_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(net_hit, 0, 0);
    lv_obj_clear_flag(net_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(net_hit, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(net_hit);
    lv_obj_add_event_cb(net_hit, ScreenCommon_OnStatusNetworkIconClicked, LV_EVENT_CLICKED, nullptr);

    out.network_label = lv_label_create(net_hit);
    lv_label_set_text(out.network_label, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(out.network_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.network_label, lv_color_black(), 0);
    lv_obj_center(out.network_label);
    lv_obj_clear_flag(out.network_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* right_icons = lv_obj_create(out.bar);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_CLICKABLE);

    out.mute_label = lv_label_create(right_icons);
    lv_label_set_text(out.mute_label, "");
    lv_obj_set_style_text_font(out.mute_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.mute_label, lv_color_black(), 0);
    lv_obj_clear_flag(out.mute_label, LV_OBJ_FLAG_CLICKABLE);

    out.battery_pct_label = lv_label_create(right_icons);
    lv_label_set_text(out.battery_pct_label, "");
    lv_obj_set_style_text_font(out.battery_pct_label, ui_font, 0);
    lv_obj_set_style_text_color(out.battery_pct_label, lv_color_black(), 0);
    lv_obj_set_style_margin_left(out.battery_pct_label, kBatteryPctMarginL, 0);
    lv_obj_set_style_translate_y(out.battery_pct_label, kBatteryPctNudgeY, 0);
    lv_obj_clear_flag(out.battery_pct_label, LV_OBJ_FLAG_CLICKABLE);

    out.battery_label = lv_label_create(right_icons);
    lv_label_set_text(out.battery_label, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(out.battery_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.battery_label, lv_color_black(), 0);
    lv_obj_set_style_margin_left(out.battery_label, kBatteryIconMarginL, 0);
    lv_obj_clear_flag(out.battery_label, LV_OBJ_FLAG_CLICKABLE);

    out.overlay = lv_obj_create(scr);
    lv_obj_set_size(out.overlay, LV_HOR_RES, out.height);
    lv_obj_set_style_radius(out.overlay, 0, 0);
    lv_obj_set_style_bg_opa(out.overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(out.overlay, 0, 0);
    lv_obj_set_style_pad_all(out.overlay, 0, 0);
    lv_obj_set_style_pad_top(out.overlay, kStatusPadV, 0);
    lv_obj_set_style_pad_bottom(out.overlay, kStatusPadV, 0);
    lv_obj_set_scrollbar_mode(out.overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(out.overlay, LV_LAYOUT_NONE, 0);
    lv_obj_align(out.overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(out.overlay, LV_OBJ_FLAG_CLICKABLE);

    const lv_coord_t status_text_w = LV_HOR_RES - kStatusTextReserveW;

    out.notification_label = lv_label_create(out.overlay);
    lv_obj_set_width(out.notification_label, status_text_w);
    lv_label_set_long_mode(out.notification_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(out.notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(out.notification_label, ui_font, 0);
    lv_obj_set_style_text_color(out.notification_label, lv_color_black(), 0);
    lv_label_set_text(out.notification_label, "");
    lv_obj_align(out.notification_label, LV_ALIGN_CENTER, 0, kStatusTextNudgeY);
    lv_obj_add_flag(out.notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(out.notification_label, LV_OBJ_FLAG_CLICKABLE);

    out.status_label = lv_label_create(out.overlay);
    lv_obj_set_width(out.status_label, status_text_w);
    lv_label_set_long_mode(out.status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(out.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(out.status_label, ui_font, 0);
    lv_obj_set_style_text_color(out.status_label, lv_color_black(), 0);
    lv_label_set_text(out.status_label, "");
    lv_obj_align(out.status_label, LV_ALIGN_CENTER, 0, kStatusTextNudgeY);
    lv_obj_clear_flag(out.status_label, LV_OBJ_FLAG_CLICKABLE);

    out.low_battery_popup = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(out.low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(out.low_battery_popup, LV_HOR_RES * 9 / 10, status_line_h * 2);
    lv_obj_align(out.low_battery_popup, LV_ALIGN_BOTTOM_MID, 0, -kLowBatteryBottom);
    lv_obj_set_style_bg_color(out.low_battery_popup, lv_color_black(), 0);
    lv_obj_set_style_radius(out.low_battery_popup, kLowBatteryRadius, 0);
    lv_obj_t* low_battery_label = lv_label_create(out.low_battery_popup);
    lv_label_set_text(low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_font(low_battery_label, ui_font, 0);
    lv_obj_set_style_text_color(low_battery_label, lv_color_white(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(out.low_battery_popup, LV_OBJ_FLAG_HIDDEN);

    if (auto* disp = LvAdapterEpdiy::Instance()) {
        disp->BindStatusBar(out.status_label, out.notification_label, out.network_label,
                            out.battery_label, out.battery_pct_label, out.mute_label);
    }

    return out;
}

namespace {

// I1 无真灰阶：弹窗外遮罩用大颗粒网点（16x16 平铺，3x3 黑块）。
constexpr int kDotBackdropW = 16;
constexpr int kDotBackdropH = 16;
constexpr int kDotSize = 3;
constexpr int kDotPitch = 8;
uint8_t s_dot_backdrop_l8[kDotBackdropW * kDotBackdropH];
lv_image_dsc_t s_dot_backdrop_img;
bool s_dot_backdrop_ready = false;

const lv_image_dsc_t* DotBackdropImg() {
    if (!s_dot_backdrop_ready) {
        for (int i = 0; i < kDotBackdropW * kDotBackdropH; ++i) {
            s_dot_backdrop_l8[i] = 0xFF;
        }
        for (int gy = 0; gy < kDotBackdropH; gy += kDotPitch) {
            const int x0 = ((gy / kDotPitch) & 1) ? (kDotPitch / 2) : 2;
            for (int gx = x0; gx < kDotBackdropW; gx += kDotPitch) {
                for (int dy = 0; dy < kDotSize; ++dy) {
                    for (int dx = 0; dx < kDotSize; ++dx) {
                        const int x = (gx + dx) % kDotBackdropW;
                        const int y = (gy + 2 + dy) % kDotBackdropH;
                        s_dot_backdrop_l8[y * kDotBackdropW + x] = 0x00;
                    }
                }
            }
        }
        s_dot_backdrop_img.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dot_backdrop_img.header.cf = LV_COLOR_FORMAT_L8;
        s_dot_backdrop_img.header.flags = 0;
        s_dot_backdrop_img.header.w = kDotBackdropW;
        s_dot_backdrop_img.header.h = kDotBackdropH;
        s_dot_backdrop_img.header.stride = kDotBackdropW;
        s_dot_backdrop_img.data_size = sizeof(s_dot_backdrop_l8);
        s_dot_backdrop_img.data = s_dot_backdrop_l8;
        s_dot_backdrop_ready = true;
    }
    return &s_dot_backdrop_img;
}

}  // namespace

void ScreenApplyDotBackdrop(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(obj, DotBackdropImg(), 0);
    lv_obj_set_style_bg_image_tiled(obj, true, 0);
}
