// assistant_ptt.cc — split from assistant_screen.cc
#include "assistant_screen_priv.h"

#include "assistant_ptt.h"
#include "application.h"
#include "boot_key_handler.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"

#include <cstring>

#include <esp_log.h>

// PTT 任一源仍按住（BOOT 任意按下 ∪ 屏触已臂听）。跨线程可读。
bool Assistant_IsAnyPttHeld() {
    return BootKey_IsHeld() || Assistant_State().ptt.touch_ptt_held.load(std::memory_order_acquire);
}

// PTT 状态栏波形：仍按住，且已进 Listening（联网/通道就绪后），Connecting 期间不显示。
bool Assistant_IsPttWaveWanted() {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
        return false;
    }
    if (Assistant_State().ptt.touch_ptt_held.load(std::memory_order_acquire)) {
        return true;
    }
    return BootKey_IsHeld() && BootKey_DidLongPress();
}

// 某一 PTT 源刚松开后：若其它源仍持有则保持聆听，否则 StopListening。
void Assistant_StopListeningIfNoPttHeld(const char* why) {
    Assistant_RequestSyncPttOverlay();
    if (Assistant_IsAnyPttHeld()) {
        ESP_LOGI(TAG, "%s: other PTT still held, keep listening", why);
        return;
    }
    ESP_LOGI(TAG, "%s -> StopListening", why);
    Application::GetInstance().StopListening();
}

void Assistant_CancelTouchPttArmTimer() {
    if (Assistant_State().ptt.touch_ptt_arm_timer == nullptr) {
        return;
    }
    lv_timer_delete(Assistant_State().ptt.touch_ptt_arm_timer);
    Assistant_State().ptt.touch_ptt_arm_timer = nullptr;
}

void Assistant_ResetTouchPttState() {
    Assistant_CancelTouchPttArmTimer();
    Assistant_State().ptt.touch_ptt_finger_down = false;
    Assistant_State().ptt.touch_suppress_click = false;
    Assistant_State().ptt.touch_ptt_held.store(false, std::memory_order_release);
}

void ArmTouchPttFromTimer(lv_timer_t* /*t*/) {
    // one-shot：先摘掉句柄，避免 Release/Unload 二次 delete
    Assistant_State().ptt.touch_ptt_arm_timer = nullptr;
    if (!Assistant_State().active.load(std::memory_order_acquire) || !Assistant_State().ptt.touch_ptt_finger_down) {
        return;
    }
    // 重绘堵住 LVGL 时 timer 可能迟到：抬起已进 touch_feed 则丢弃
    if (!TouchUiFingerIsDown()) {
        ESP_LOGI(TAG, "touch PTT arm ignored: finger already up");
        Assistant_State().ptt.touch_ptt_finger_down = false;
        return;
    }
    if (Assistant_State().ptt.touch_ptt_held.exchange(true, std::memory_order_acq_rel)) {
        return;  // 已臂听（重复 timer 不应发生）
    }
    Assistant_State().ptt.touch_suppress_click = true;
    // 按下已由 HapticAttachClick 早震，臂听不再二次震（与阅读长按出浮层同）
    ESP_LOGI(TAG, "touch long-press (%ums) -> StartListening (same as BOOT PTT)",
             static_cast<unsigned>(kTouchPttLongPressMs));
    Assistant_RequestSyncPttOverlay();
    Application::GetInstance().StartListening();
}

void OnTouchPttPressed() {
    if (!Assistant_State().active.load(std::memory_order_acquire)) {
        return;
    }
    Assistant_State().ptt.touch_ptt_finger_down = true;
    Assistant_CancelTouchPttArmTimer();
    Assistant_State().ptt.touch_ptt_arm_timer =
        lv_timer_create(ArmTouchPttFromTimer, kTouchPttLongPressMs, nullptr);
    if (Assistant_State().ptt.touch_ptt_arm_timer == nullptr) {
        ESP_LOGW(TAG, "touch PTT: lv_timer_create failed");
        Assistant_State().ptt.touch_ptt_finger_down = false;
        return;
    }
    lv_timer_set_repeat_count(Assistant_State().ptt.touch_ptt_arm_timer, 1);
}

void OnTouchPttReleased(const char* why) {
    Assistant_State().ptt.touch_ptt_finger_down = false;
    Assistant_CancelTouchPttArmTimer();
    if (!Assistant_State().ptt.touch_ptt_held.exchange(false, std::memory_order_acq_rel)) {
        return;  // 未满阈值抬起：由 CLICKED 处理翻页
    }
    Assistant_StopListeningIfNoPttHeld(why);
}

bool TouchEventPoint(lv_event_t* e, lv_point_t* out) {
    if (out == nullptr) {
        return false;
    }
    lv_indev_t* indev = e != nullptr ? lv_event_get_indev(e) : nullptr;
    if (indev == nullptr) {
        indev = lv_indev_active();
    }
    if (indev == nullptr || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return false;
    }
    lv_indev_get_point(indev, out);
    return true;
}

// dir: -1 上一页，+1 下一页。已在首页时上一页返回 false；末页下一页 no-op 仍返回 true。
bool Assistant_TurnPage(int dir) {
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        if (!Assistant_State().layout.has_a2ui_content || Assistant_State().layout.pages.empty()) {
            return dir > 0;
        }
        if (dir < 0) {
            if (Assistant_State().layout.page_index <= 0) {
                return false;
            }
            --Assistant_State().layout.page_index;
            ESP_LOGI(TAG, "page turn -> %d/%d", Assistant_State().layout.page_index + 1, static_cast<int>(Assistant_State().layout.pages.size()));
        } else if (dir > 0) {
            if (Assistant_State().layout.page_index + 1 < static_cast<int>(Assistant_State().layout.pages.size())) {
                ++Assistant_State().layout.page_index;
                ESP_LOGI(TAG, "page turn -> %d/%d", Assistant_State().layout.page_index + 1, static_cast<int>(Assistant_State().layout.pages.size()));
            } else {
                return true;
            }
        } else {
            return false;
        }
    }
    Assistant_RequestRenderCurrentPage();
    return true;
}

void OnTouchPageClicked(lv_event_t* e) {
    if (lv_event_get_target(e) != Assistant_State().ptt.touch_ptt_hit) {
        return;
    }
    if (Assistant_State().ptt.touch_suppress_click) {
        Assistant_State().ptt.touch_suppress_click = false;
        return;
    }
    if (!Assistant_State().layout.has_a2ui_content || Assistant_State().layout.pages.empty()) {
        return;
    }
    // 震动已在按下早震；此处只投递翻页（与阅读：AttachClick 早震 + CLICKED 换页 一致）
    // 左 1/3 上一页，右 2/3 下一页；首页再点左区停住，不退出
    lv_point_t pt = {};
    const int dir = (TouchEventPoint(e, &pt) && pt.x < LV_HOR_RES / 3) ? -1 : 1;
    Assistant_TurnPage(dir);
}

void OnTouchPttEvent(lv_event_t* e) {
    if (lv_event_get_target(e) != Assistant_State().ptt.touch_ptt_hit) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        OnTouchPttPressed();
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        OnTouchPttReleased(code == LV_EVENT_PRESS_LOST ? "touch press-lost"
                                                       : "touch press-up");
    }
}

lv_obj_t* Assistant_CreateTouchPttHitLayer(lv_obj_t* scr) {
    // 全屏透明 hit：叠在内容之上吃 pointer；非 clickable 的顶栏字/波形仍可穿透到本层。
    // 盖板虚拟键在 touch_feed 坐标命中，不走 LVGL hit，故不受本层影响。
    lv_obj_t* hit = lv_obj_create(scr);
    lv_obj_set_size(hit, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(hit, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_set_style_pad_all(hit, 0, 0);
    lv_obj_set_style_radius(hit, 0, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_GESTURE_BUBBLE);
    Assistant_DisableScroll(hit);
    // 页内热区：touch_feed 按下早震；短按翻页 / 长按 PTT 业务侧勿再 Pulse
    HapticAttachClick(hit);
    lv_obj_add_event_cb(hit, OnTouchPttEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(hit, OnTouchPttEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(hit, OnTouchPttEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(hit, OnTouchPageClicked, LV_EVENT_CLICKED, nullptr);
    return hit;
}

lv_coord_t PttWaveBarHeight(uint8_t curve_v) {
    if (Assistant_State().ptt.ptt_wave_max_h <= 0) {
        return 2;
    }
    lv_coord_t h = static_cast<lv_coord_t>((static_cast<int>(curve_v) * Assistant_State().ptt.ptt_wave_max_h) / 44);
    if (h < 2) {
        h = 2;
    }
    if (h > Assistant_State().ptt.ptt_wave_max_h) {
        h = Assistant_State().ptt.ptt_wave_max_h;
    }
    return h;
}

lv_obj_t* CreatePttWaveBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_width(bar, kPttWaveBarW);
    lv_obj_set_height(bar, 2);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(bar);
    return bar;
}

lv_obj_t* Assistant_CreatePttWaveHost(lv_obj_t* parent, lv_coord_t bar_h) {
    lv_obj_t* host = lv_obj_create(parent);
    lv_obj_set_size(host, LV_SIZE_CONTENT, bar_h);
    lv_obj_set_style_bg_opa(host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(host, 0, 0);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_set_style_pad_column(host, kPttWaveBarGap, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(host);
    for (int i = 0; i < kPttWaveBars; ++i) {
        Assistant_State().ptt.ptt_wave_bars[i] = CreatePttWaveBar(host);
    }
    return host;
}

void UpdatePttWaveBars() {
    constexpr int kCenter = kPttWaveBars / 2;
    for (int i = 0; i < kPttWaveBars; ++i) {
        if (Assistant_State().ptt.ptt_wave_bars[i] == nullptr || !lv_obj_is_valid(Assistant_State().ptt.ptt_wave_bars[i])) {
            continue;
        }
        const int dist = (i >= kCenter) ? (i - kCenter) : (kCenter - i);
        const int phase = (Assistant_State().ptt.ptt_wave_frame + dist) % kPttWaveFrames;
        lv_obj_set_height(Assistant_State().ptt.ptt_wave_bars[i], PttWaveBarHeight(kPttWaveCurve[phase]));
    }
}

void OnPttWaveTimer(lv_timer_t* /*t*/) {
    if (!Assistant_State().ptt.ptt_wave_visible) {
        return;
    }
    Assistant_State().ptt.ptt_wave_frame = static_cast<uint8_t>((Assistant_State().ptt.ptt_wave_frame + 1) % kPttWaveFrames);
    UpdatePttWaveBars();
}

void Assistant_StopPttWaveAnim() {
    if (Assistant_State().ptt.ptt_wave_timer != nullptr) {
        lv_timer_delete(Assistant_State().ptt.ptt_wave_timer);
        Assistant_State().ptt.ptt_wave_timer = nullptr;
    }
    Assistant_State().ptt.ptt_wave_frame = 0;
}

void StartPttWaveAnim() {
    Assistant_StopPttWaveAnim();
    for (int i = 0; i < kPttWaveBars; ++i) {
        if (Assistant_State().ptt.ptt_wave_bars[i] == nullptr || !lv_obj_is_valid(Assistant_State().ptt.ptt_wave_bars[i])) {
            return;
        }
    }
    UpdatePttWaveBars();
    Assistant_State().ptt.ptt_wave_timer = lv_timer_create(OnPttWaveTimer, kPttWaveFrameMs, nullptr);
    if (Assistant_State().ptt.ptt_wave_timer == nullptr) {
        ESP_LOGW(TAG, "StartPttWaveAnim: lv_timer_create failed");
    }
}

void HideStatusCenterText() {
    if (Assistant_State().chrome.status_label != nullptr && lv_obj_is_valid(Assistant_State().chrome.status_label)) {
        lv_obj_add_flag(Assistant_State().chrome.status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (Assistant_State().chrome.notification_label != nullptr && lv_obj_is_valid(Assistant_State().chrome.notification_label)) {
        lv_obj_add_flag(Assistant_State().chrome.notification_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void RestoreStatusCenterText() {
    if (Assistant_State().chrome.status_label != nullptr && lv_obj_is_valid(Assistant_State().chrome.status_label)) {
        lv_obj_remove_flag(Assistant_State().chrome.status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_ApplyPttWave(bool show) {
    if (Assistant_State().ptt.ptt_wave == nullptr || !lv_obj_is_valid(Assistant_State().ptt.ptt_wave)) {
        return;
    }
    if (!AssistantScreen::IsActive()) {
        return;
    }
    if (show == Assistant_State().ptt.ptt_wave_visible) {
        return;
    }
    Assistant_State().ptt.ptt_wave_visible = show;
    if (show) {
        HideStatusCenterText();
        lv_obj_remove_flag(Assistant_State().ptt.ptt_wave, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(Assistant_State().ptt.ptt_wave);
        StartPttWaveAnim();
    } else {
        Assistant_StopPttWaveAnim();
        lv_obj_add_flag(Assistant_State().ptt.ptt_wave, LV_OBJ_FLAG_HIDDEN);
        RestoreStatusCenterText();
    }
}

void AsyncSyncPttOverlay(void* /*arg*/) {
    Assistant_ApplyPttWave(Assistant_IsPttWaveWanted());
}

void Assistant_RequestSyncPttOverlay() {
    // BOOT/触摸回调不在 LVGL 任务：经 ScreenLvAsync，勿在本线程死等锁
    ScreenLvAsync(AsyncSyncPttOverlay);
}

