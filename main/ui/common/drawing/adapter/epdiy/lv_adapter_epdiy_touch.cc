/**
 * @file lv_adapter_epdiy_touch.cc
 * @brief LVGL 触摸：盖板 VK + 屏内早震（独立 TU，避免 epdiy 大文件触发 GCC ICE）
 */

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/runtime/input/screen_gesture.h"
#include "lv_adapter_epdiy_touch.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "config.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "boards/common/input/touch/ft6336u/touch_input.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "sysmgmt/update/update_presentation.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/drawing/ui_scale.h"

#define TAG "epdiy_touch"

/** @brief 逻辑坐标 → UI：扣 content inset 再加板级微调；越界返回 false */
static bool MapLogicalToUi(int lx, int ly, int* ux, int* uy, bool clamp_border = false) {
    int x, y;
    DisplayUiTouchToLogical(lx + DISPLAY_TOUCH_OFFSET_X, ly + DISPLAY_TOUCH_OFFSET_Y, &x, &y);
    // 仅手势起点接纳屏幕留白，普通点击仍限制在内容区域。
    if (clamp_border && lx >= 0 && lx < DISPLAY_WIDTH && ly >= 0 && ly < DISPLAY_HEIGHT) {
        x = std::clamp(x, 0, DisplayUiLogW() - 1);
        y = std::clamp(y, 0, DisplayUiLogH() - 1);
    }
    if (x < 0 || x >= DisplayUiLogW() || y < 0 || y >= DisplayUiLogH()) {
        return false;
    }
    *ux = x;
    *uy = y;
    return true;
}

static const char* s_vk_hold_name = nullptr;
static int64_t s_vk_hold_down_us = 0;
static bool s_vk_hold_long = false;
static std::atomic<bool> s_ui_finger_down{false};

struct CoverKey {
    const char* name;
    int x;
};

static constexpr CoverKey kCoverKeys[] = {
    {"vk_home", TOUCH_VK_HOME_X},
    {"vk_prev", TOUCH_VK_PREV_X},
    {"vk_next", TOUCH_VK_NEXT_X},
};

static const char* HitCoverKey(int raw_x, int raw_y) {
    if (raw_y < TOUCH_VK_Y - TOUCH_VK_TOL || raw_y > TOUCH_VK_Y + TOUCH_VK_TOL) {
        return nullptr;
    }
    for (const CoverKey& k : kCoverKeys) {
        const int dx = raw_x - k.x;
        if (dx >= -TOUCH_VK_TOL && dx <= TOUCH_VK_TOL) {
            return k.name;
        }
    }
    return nullptr;
}

static void EndCoverHold(bool emit) {
    if (s_vk_hold_name == nullptr) {
        return;
    }
    if (emit) {
        const char* name = s_vk_hold_name;
        if (!s_vk_hold_long) {
            HapticPulseIfEnabled();
        }
        VkKey_PostTouch(VkTouchPhase::kUp, name);
    }
    s_vk_hold_name = nullptr;
    s_vk_hold_down_us = 0;
    s_vk_hold_long = false;
}

static void BeginCoverHold(const char* vk, int raw_x, int raw_y) {
    s_vk_hold_name = vk;
    s_vk_hold_down_us = esp_timer_get_time();
    s_vk_hold_long = false;
    HapticBeginPress();
    VkKey_PostTouch(VkTouchPhase::kDown, vk);
    ESP_LOGD(TAG, "cover key [%s] down raw=(%d,%d)", vk, raw_x, raw_y);
}

static void OnTouchSample(uint8_t count, int raw_x, int raw_y, int lx, int ly) {
    if (count == 0) {
        EndCoverHold(true);
        touch_input_set_ui_suppress(false);
        s_ui_finger_down = false;
        return;
    }

    PowerPolicy::GetInstance().NotifyUserActivity();

    const char* vk = HitCoverKey(raw_x, raw_y);
    if (vk != nullptr) {
        touch_input_set_ui_suppress(true);
        s_ui_finger_down = false;
        if (s_vk_hold_name == nullptr) {
            BeginCoverHold(vk, raw_x, raw_y);
        } else if (std::strcmp(s_vk_hold_name, vk) != 0) {
            EndCoverHold(true);
            BeginCoverHold(vk, raw_x, raw_y);
        } else if (!s_vk_hold_long) {
            const int64_t elapsed_ms = (esp_timer_get_time() - s_vk_hold_down_us) / 1000;
            if (elapsed_ms >= kBootLongPressMs) {
                s_vk_hold_long = true;
                ESP_LOGD(TAG, "cover key [%s] long-press", s_vk_hold_name);
                VkKey_PostTouch(VkTouchPhase::kLong, s_vk_hold_name);
            }
        }
        return;
    }

    touch_input_set_ui_suppress(false);
    EndCoverHold(true);

    int ux = 0;
    int uy = 0;
    if (!MapLogicalToUi(lx, ly, &ux, &uy)) {
        s_ui_finger_down = false;
        return;
    }
    if (!s_ui_finger_down) {
        s_ui_finger_down = true;
        HapticBeginPress();
        HapticTryPulseAtUiPoint(static_cast<int16_t>(ux), static_cast<int16_t>(uy));
        ESP_LOGD(TAG, "touch down ui=%d,%d", ux, uy);
    }
}

bool TouchUiFingerIsDown() {
    return s_ui_finger_down;
}

bool TouchUiHasPendingEdges() {
    return touch_input_has_pending_edges();
}

void LvAdapterEpdiy_TouchBind(void) {
    touch_input_set_sample_cb(OnTouchSample);
}

void LvAdapterEpdiy_TouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    static bool tracking = false;
    static bool consumed = false;
    static bool close_panel = false;
    static bool ignore_until_release = false;
    static lv_point_t origin = {};
    static lv_obj_t* start_screen = nullptr;
    touch_input_point_t p = {};
    const bool pressed = touch_input_read(&p);
    const bool blocked = StandbyPresentation::IsActive() || UiInputQuickPanelBlocked();
    int swipe_x = 0, swipe_y = 0;
    int swipe_start_x = 0, swipe_start_y = 0;
    const bool swipe_valid = !blocked &&
        MapLogicalToUi(p.x, p.y, &swipe_x, &swipe_y, true) &&
        MapLogicalToUi(p.start_x, p.start_y, &swipe_start_x, &swipe_start_y, true);
    lv_point_t swipe_point = {swipe_x, swipe_y};
    if (ScreenHandleHorizontalSwipe(indev, pressed, swipe_point,
                                    {swipe_start_x, swipe_start_y}, swipe_valid)) {
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = touch_input_has_pending_edges();
        return;
    }
    if (blocked || (tracking && start_screen != lv_screen_active())) {
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
    }
    int ux = 0;
    int uy = 0;
    if ((pressed || tracking) && !MapLogicalToUi(p.x, p.y, &ux, &uy)) {
        // 从留白起滑时等待手指进入内容区，不将整次触摸标为无效。
        if (pressed && !blocked && !tracking && !ignore_until_release && MapLogicalToUi(p.x, p.y, &ux, &uy, true)) {
            data->state = LV_INDEV_STATE_RELEASED;
            data->continue_reading = touch_input_has_pending_edges();
            return;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
        return;
    }
    if (pressed && !blocked && !ignore_until_release && !tracking) {
        tracking = true;
        int start_x = 0, start_y = 0;
        if (!MapLogicalToUi(p.start_x, p.start_y, &start_x, &start_y, true)) {
            data->state = LV_INDEV_STATE_RELEASED;
            tracking = false;
            ignore_until_release = true;
            return;
        }
        origin = {static_cast<int32_t>(start_x), static_cast<int32_t>(start_y)};
        start_screen = lv_screen_active();
        close_panel = ScreenFrontlightIsVisible();
        if ((close_panel && ScreenFrontlightOwnsDrag(origin.x, origin.y)) ||
            (!close_panel && ScreenPageOwnsVerticalDrag(origin))) {
            tracking = false;
            ignore_until_release = true;
        }
    }
    if (tracking && !consumed) {
        const int dy = uy - origin.y;
        const int distance = close_panel ? -dy : dy;
        if (distance >= UiSy(60) && distance > 2 * std::abs(ux - origin.x)) {
            consumed = true;
            lv_indev_wait_release(indev);
        }
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (consumed && !blocked) {
            // 松手后再创建遮罩，避免当前手势继续操作新面板。
            if (close_panel) {
                if (lv_async_call([](void*) { ScreenFrontlightHide(); }, nullptr) != LV_RESULT_OK)
                    ESP_LOGW(TAG, "quick panel close request failed");
            } else {
                if (lv_async_call(
                        [](void* screen) {
                            if (screen == lv_screen_active()) {
                                ScreenFrontlightShow();
                            }
                        },
                        start_screen) != LV_RESULT_OK)
                    ESP_LOGW(TAG, "quick panel open request failed");
            }
        }
        tracking = false;
        consumed = false;
        ignore_until_release = false;
        return;
    }
    if (consumed) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = touch_input_has_pending_edges();
        return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point = swipe_point;
    // 短按回放：同一次 indev 周期内立刻再读到 RELEASED，才能生成 CLICKED
    if (touch_input_has_pending_edges()) {
        data->continue_reading = true;
    }
}
