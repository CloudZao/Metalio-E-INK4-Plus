#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <esp_log.h>
#include <font_awesome.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/widgets/screen_common_priv.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"

LV_FONT_DECLARE(font_awesome_30_1);

// I1 无真灰阶：弹窗外遮罩用大颗粒网点（16x16 平铺，3x3 黑块）。
static constexpr int kDotBackdropW = 16;
static constexpr int kDotBackdropH = 16;
static constexpr int kDotSize = 3;
static constexpr int kDotPitch = 8;
static uint8_t s_dot_backdrop_l8[kDotBackdropW * kDotBackdropH];
static lv_image_dsc_t s_dot_backdrop_img;
static bool s_dot_backdrop_ready = false;

struct ScreenDialogState {
    lv_obj_t* mask = nullptr;
    ScreenDialogAction actions[3];
};

static lv_obj_t* s_choice_dialog = nullptr;
static UiModal s_choice_modal;

static void ScreenDialogDeleted(lv_event_t* event) {
    auto* state = static_cast<ScreenDialogState*>(lv_event_get_user_data(event));
    if (s_choice_dialog == state->mask) {
        s_choice_dialog = nullptr;
    }
    delete state;
}

static void ScreenDialogActionClicked(lv_event_t* event) {
    auto* action = static_cast<ScreenDialogAction*>(lv_event_get_user_data(event));
    auto run = std::move(action->run);
    ScreenDismissChoiceDialog();
    if (run) {
        try {
            run();
        } catch (const std::bad_alloc&) {
            ESP_LOGE("ScreenDialog", "operation=action result=no_memory");
            ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
        }
    }
}

static const lv_image_dsc_t* DotBackdropImg() {
    static bool was_dark = false;
    const bool dark = UiThemeIsDark();
    if (!s_dot_backdrop_ready || was_dark != dark) {
        lv_image_cache_drop(&s_dot_backdrop_img);
        was_dark = dark;
        for (int i = 0; i < kDotBackdropW * kDotBackdropH; ++i) {
            s_dot_backdrop_l8[i] = dark ? 0x00 : 0xFF;
        }
        for (int gy = 0; gy < kDotBackdropH; gy += kDotPitch) {
            const int x0 = ((gy / kDotPitch) & 1) ? (kDotPitch / 2) : 2;
            for (int gx = x0; gx < kDotBackdropW; gx += kDotPitch) {
                for (int dy = 0; dy < kDotSize; ++dy) {
                    for (int dx = 0; dx < kDotSize; ++dx) {
                        const int x = (gx + dx) % kDotBackdropW;
                        const int y = (gy + 2 + dy) % kDotBackdropH;
                        s_dot_backdrop_l8[y * kDotBackdropW + x] = dark ? 0xFF : 0x00;
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

static void ScreenStatusFitText(lv_event_t* event) {
    auto* right_icons = static_cast<lv_obj_t*>(lv_event_get_target(event));
    auto* overlay = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    const int reserve = std::max<int>(kStatusIconSlotW, lv_obj_get_width(right_icons)) + kStatusPadH + UiSx(4);
    const int width = std::max<int>(1, lv_obj_get_width(lv_obj_get_parent(right_icons)) - reserve * 2);
    for (uint32_t i = 0; i < lv_obj_get_child_count(overlay); ++i) {
        lv_obj_set_width(lv_obj_get_child(overlay, i), width);
    }
}

lv_coord_t ScreenStatusBarHeight() {
    const lv_font_t* font = UiFont();
    return (font != nullptr ? font->line_height : 30) + kStatusPadV * 2;
}

EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr) {
    EpdStatusBar out;

    const lv_font_t* ui_font = UiFont();
    out.height = ScreenStatusBarHeight();
    const lv_coord_t status_line_h = out.height - kStatusPadV * 2;
    const lv_coord_t icon_row_h = std::max(status_line_h, kStatusNetHitMinH);

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
    lv_obj_set_flex_align(out.bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(out.bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(out.bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(out.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(out.bar, LV_OBJ_FLAG_SCROLLABLE);

    // 底边横线挂 bar 下（FLOATING 不进 flex）；藏 bar 时一并隐藏。左右让出 pad。
    constexpr lv_coord_t kStatusRuleH = 2;
    lv_obj_t* rule = lv_obj_create(out.bar);
    lv_obj_remove_style_all(rule);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(rule, lv_pct(100), kStatusRuleH);
    lv_obj_set_style_bg_color(rule, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    // content 坐标贴外底：越过 pad_bottom
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, kStatusPadV);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* net_hit = lv_obj_create(out.bar);
    lv_obj_remove_style_all(net_hit);
    lv_obj_set_size(net_hit, kStatusIconSlotW, icon_row_h);
    lv_obj_set_style_bg_opa(net_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(net_hit, 0, 0);
    lv_obj_clear_flag(net_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(net_hit, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(net_hit);
    lv_obj_add_event_cb(net_hit, [](lv_event_t*) { ScreenOpenNetworkSettings(); }, LV_EVENT_CLICKED, nullptr);

    out.network_label = lv_label_create(net_hit);
    lv_label_set_text(out.network_label, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(out.network_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.network_label, UiColorForeground(), 0);
    lv_obj_center(out.network_label);
    lv_obj_clear_flag(out.network_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* right_icons = lv_obj_create(out.bar);
    lv_obj_remove_style_all(right_icons);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, icon_row_h);
    lv_obj_set_style_pad_column(right_icons, kStatusIconGap, 0);
    // 对称留白放在电量组外侧，百分比与电池之间仅保留列间距。
    lv_font_glyph_dsc_t wifi_glyph{};
    if (lv_font_get_glyph_dsc(&font_awesome_30_1, &wifi_glyph, 0xf1eb, 0)) {
        lv_obj_set_style_pad_right(right_icons, std::max<int>(0, (kStatusIconSlotW - wifi_glyph.box_w) / 2), 0);
    }
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_SCROLLABLE);

    out.mute_label = lv_label_create(right_icons);
    lv_label_set_text(out.mute_label, "");
    lv_obj_set_style_text_font(out.mute_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.mute_label, UiColorForeground(), 0);
    lv_obj_clear_flag(out.mute_label, LV_OBJ_FLAG_CLICKABLE);

    out.battery_pct_label = lv_label_create(right_icons);
    lv_label_set_text(out.battery_pct_label, "");
    lv_obj_set_style_text_font(out.battery_pct_label, ui_font, 0);
    lv_obj_set_style_text_color(out.battery_pct_label, UiColorForeground(), 0);
    lv_obj_set_style_translate_y(out.battery_pct_label, kBatteryPctNudgeY, 0);
    lv_obj_clear_flag(out.battery_pct_label, LV_OBJ_FLAG_CLICKABLE);

    out.battery_label = lv_label_create(right_icons);
    lv_label_set_text(out.battery_label, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(out.battery_label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(out.battery_label, UiColorForeground(), 0);
    // 补偿固定图标字体可见中心与 LVGL 居中取整的差异。
    lv_obj_set_style_translate_y(out.battery_label, 1, 0);
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
    lv_obj_clear_flag(out.overlay, LV_OBJ_FLAG_SCROLLABLE);

    out.notification_label = lv_label_create(out.overlay);
    lv_label_set_long_mode(out.notification_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(out.notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(out.notification_label, ui_font, 0);
    lv_obj_set_style_text_color(out.notification_label, UiColorForeground(), 0);
    lv_label_set_text(out.notification_label, "");
    lv_obj_align(out.notification_label, LV_ALIGN_CENTER, 0, kStatusTextNudgeY);
    lv_obj_add_flag(out.notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(out.notification_label, LV_OBJ_FLAG_CLICKABLE);

    out.status_label = lv_label_create(out.overlay);
    lv_label_set_long_mode(out.status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(out.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(out.status_label, ui_font, 0);
    lv_obj_set_style_text_color(out.status_label, UiColorForeground(), 0);
    lv_label_set_text(out.status_label, "");
    lv_obj_align(out.status_label, LV_ALIGN_CENTER, 0, kStatusTextNudgeY);
    lv_obj_clear_flag(out.status_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(right_icons, ScreenStatusFitText, LV_EVENT_SIZE_CHANGED, out.overlay);
    lv_obj_add_event_cb(out.bar, [](lv_event_t* event) {
        lv_obj_send_event(static_cast<lv_obj_t*>(lv_event_get_user_data(event)), LV_EVENT_SIZE_CHANGED, nullptr);
    }, LV_EVENT_SIZE_CHANGED, right_icons);
    lv_obj_update_layout(out.bar);
    lv_obj_send_event(right_icons, LV_EVENT_SIZE_CHANGED, nullptr);

    out.low_battery_popup = lv_obj_create(scr);
    lv_obj_clear_flag(out.low_battery_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(out.low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(out.low_battery_popup, LV_HOR_RES * 9 / 10, status_line_h * 2);
    lv_obj_align(out.low_battery_popup, LV_ALIGN_BOTTOM_MID, 0, -kLowBatteryBottom);
    lv_obj_set_style_bg_color(out.low_battery_popup, UiColorForeground(), 0);
    lv_obj_set_style_radius(out.low_battery_popup, kLowBatteryRadius, 0);
    lv_obj_t* low_battery_label = lv_label_create(out.low_battery_popup);
    lv_label_set_text(low_battery_label, LangString_BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_font(low_battery_label, ui_font, 0);
    lv_obj_set_style_text_color(low_battery_label, UiColorBackground(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(out.low_battery_popup, LV_OBJ_FLAG_HIDDEN);

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusBar(out.status_label, out.notification_label, out.network_label, out.battery_label,
                            out.battery_pct_label, out.mute_label);
    }
    lv_obj_add_event_cb(
        out.status_label,
        [](lv_event_t* event) {
            auto* display = LVAdapterDisplay::Instance();
            if (display && display->GetBoundStatusBar().status_label == lv_event_get_target(event)) {
                // 只撤销被删除控件自己的绑定，不能清掉新页面或浮层已接管的状态栏。
                display->BindStatusBar(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
        },
        LV_EVENT_DELETE, nullptr);

    return out;
}

lv_obj_t* ScreenCreateToast(lv_obj_t* parent, const char* text) {
    auto* notice = lv_label_create(parent);
    lv_obj_remove_style_all(notice);
    lv_label_set_text(notice, text ? text : "");
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(notice, LV_HOR_RES - 32, 0);
    lv_obj_set_style_text_font(notice, UiFont(), 0);
    lv_obj_set_style_text_color(notice, UiColorBackground(), 0);
    lv_obj_set_style_bg_color(notice, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notice, 8, 0);
    lv_obj_set_style_pad_hor(notice, 12, 0);
    lv_obj_set_style_pad_ver(notice, 8, 0);
    lv_obj_clear_flag(notice, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, ScreenStatusBarHeight() + 8);
    return notice;
}

void ScreenShowToast(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    static lv_obj_t* notice = nullptr;
    if (notice != nullptr) {
        lv_obj_delete(notice);
    }
    notice = ScreenCreateToast(lv_layer_top(), text);
    lv_obj_add_event_cb(notice, [](lv_event_t*) { notice = nullptr; }, LV_EVENT_DELETE, nullptr);
    lv_obj_delete_delayed(notice, 3000);
}

void ScreenApplyDotBackdrop(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(obj, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(obj, DotBackdropImg(), 0);
    lv_obj_set_style_bg_image_tiled(obj, true, 0);
}

void ScreenStyleDialogCard(lv_obj_t* card, lv_coord_t padding) {
    lv_obj_set_style_text_font(card, UiFont(), 0);
    lv_obj_set_style_text_color(card, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_border_width(card, UiSx(2), 0);
    lv_obj_set_style_radius(card, UiSx(8), 0);
    lv_obj_set_style_pad_all(card, padding, 0);
    lv_obj_set_style_max_height(card, LV_VER_RES - UiSy(32), 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
}

void ScreenDismissChoiceDialog() {
    if (s_choice_dialog) {
        lv_obj_delete(s_choice_dialog);
    }
}

lv_obj_t* ScreenShowChoiceDialog(lv_obj_t* parent, const char* title, ScreenDialogAction* actions, size_t count) try {
    if (!parent || !actions || count == 0 || count > 3 || s_choice_dialog) {
        return nullptr;
    }
    auto state = std::make_unique<ScreenDialogState>();
    for (size_t i = 0; i < count; ++i) {
        state->actions[i] = std::move(actions[i]);
    }
    auto* mask = lv_obj_create(parent);
    state->mask = mask;
    s_choice_dialog = mask;
    auto* context = state.release();
    s_choice_modal.Bind(mask, [](void*) { ScreenDismissChoiceDialog(); });
    lv_obj_add_event_cb(mask, ScreenDialogDeleted, LV_EVENT_DELETE, context);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { ScreenDismissChoiceDialog(); }, LV_EVENT_CLICKED, nullptr);

    auto* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    const lv_coord_t width = std::min<int>(UiSx(280), LV_HOR_RES - UiSx(48));
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    ScreenStyleDialogCard(card, UiSx(16));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, UiSy(12), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    auto* label = lv_label_create(card);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, title);
    for (size_t i = 0; i < count; ++i) {
        auto* button = lv_obj_create(card);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, lv_pct(100), std::max<int>(UiSy(48), UiFont()->line_height + UiSy(16)));
        lv_obj_set_style_border_width(button, UiSx(2), 0);
        lv_obj_set_style_border_color(button, UiColorForeground(), 0);
        lv_obj_set_style_radius(button, UiSx(8), 0);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(button);
        lv_obj_add_event_cb(button, ScreenDialogActionClicked, LV_EVENT_CLICKED, &context->actions[i]);
        auto* text = lv_label_create(button);
        lv_label_set_text(text, context->actions[i].text);
        lv_obj_center(text);
    }
    lv_obj_update_layout(card);
    lv_obj_center(card);
    return mask;
} catch (const std::bad_alloc&) {
    ESP_LOGE("ScreenDialog", "operation=create result=no_memory");
    ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
    return nullptr;
}

void ScreenConfirmDelete(lv_obj_t* parent, std::function<void()> confirmed) {
    ScreenDialogAction actions[] = {{LangString_COMMON_DELETE, std::move(confirmed)},
                                          {LangString_COMMON_CANCEL, {}}};
    ScreenShowChoiceDialog(parent, LangString_COMMON_DELETE_CONFIRM, actions, 2);
}
