#include "screen_common.h"
#include "screen_common_ui.h"
#include "screen_common_nav.h"
#include "screen_common_priv.h"

#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
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
    if (ScreenCommon_on_home) {
        ESP_LOGI(TAG, "home ignored (already home)");
        return;
    }
    ESP_LOGI(TAG, "-> home (clear back stack depth was %d)", ScreenCommon_back_depth);
    ScreenGoHome();
}

void ScreenCommon_NavigateBackAsync(void* /*user_data*/) {
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

EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr) {
    EpdStatusBar out;

    const lv_font_t* ui_font = fontpack_lv_font_ui();
    const lv_coord_t status_line_h = ui_font != nullptr ? ui_font->line_height : 30;
    out.height = status_line_h + kStatusPadV * 2;

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

void ScreenApplyDotBackdrop(lv_obj_t* obj) {
    (void)obj;
}
