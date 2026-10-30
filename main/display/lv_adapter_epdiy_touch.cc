/**
 * @file lv_adapter_epdiy_touch.cc
 * @brief LVGL 触摸：盖板 VK + 屏内早震（独立 TU，避免 epdiy 大文件触发 GCC ICE）
 */

#include "lv_adapter_epdiy_touch.h"

#include "config.h"
#include "display_orient.h"
#include "haptic_feedback.h"
#include "metalio_touch.h"
#include "power_policy.h"
#include "vk_key_handler.h"
#include "screen_frontlight.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "standby_screen/standby_screen.h"
#include "ui_scale.h"

#include <cstdlib>
#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "epdiy_touch"

/** @brief 逻辑坐标 → UI：扣 content inset 再加板级微调；越界返回 false */
static bool MapLogicalToUi(int lx, int ly, int* ux, int* uy) {
    int x, y;
    DisplayUiTouchToLogical(lx + DISPLAY_TOUCH_OFFSET_X, ly + DISPLAY_TOUCH_OFFSET_Y, &x, &y);
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
        if (!s_vk_hold_long) HapticPulseIfEnabled();
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
        metalio_touch_set_ui_suppress(false);
        s_ui_finger_down = false;
        return;
    }

    PowerPolicy::GetInstance().NotifyUserActivity();

    const char* vk = HitCoverKey(raw_x, raw_y);
    if (vk != nullptr) {
        metalio_touch_set_ui_suppress(true);
        s_ui_finger_down = false;
        if (s_vk_hold_name == nullptr) {
            BeginCoverHold(vk, raw_x, raw_y);
        } else if (std::strcmp(s_vk_hold_name, vk) != 0) {
            EndCoverHold(true);
            BeginCoverHold(vk, raw_x, raw_y);
        } else if (!s_vk_hold_long) {
            const int64_t elapsed_ms = (esp_timer_get_time() - s_vk_hold_down_us) / 1000;
            if (elapsed_ms >= TOUCH_VK_LONG_MS) {
                s_vk_hold_long = true;
                ESP_LOGD(TAG, "cover key [%s] long-press", s_vk_hold_name);
                VkKey_PostTouch(VkTouchPhase::kLong, s_vk_hold_name);
            }
        }
        return;
    }

    metalio_touch_set_ui_suppress(false);
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
    return metalio_touch_has_pending_edges();
}

void LvAdapterEpdiy_TouchBind(void) {
    metalio_touch_set_sample_cb(OnTouchSample);
}

static lv_obj_t* FindGestureTarget(lv_obj_t* obj, lv_point_t point) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    lv_obj_transform_point(obj, &point, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2)
        return nullptr;
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_count(obj)) - 1; i >= 0; --i) {
        if (auto* child = FindGestureTarget(lv_obj_get_child(obj, i), point)) return child;
    }
    return obj;
}

static bool PageOwnsVerticalDrag(lv_point_t point) {
    lv_obj_t* layers[] = {lv_layer_sys(), lv_layer_top(), lv_screen_active()};
    for (auto* layer : layers) {
        auto* target = FindGestureTarget(layer, point);
        if (target == nullptr) continue;
        for (auto* obj = target; obj != nullptr; obj = lv_obj_get_parent(obj)) {
            const auto dir = lv_obj_get_scroll_dir(obj);
            // 仅实际存在纵向滚动范围的容器保留手势，长列表到达边界时仍优先滚动。
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && (dir & LV_DIR_VER) != 0 &&
                lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj) > 0) return true;
        }
        if (target != layer) return false;
    }
    return false;
}

void LvAdapterEpdiy_TouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    static bool tracking = false;
    static bool consumed = false;
    static bool close_panel = false;
    static bool ignore_until_release = false;
    static lv_point_t origin = {};
    static lv_obj_t* start_screen = nullptr;
    metalio_touch_point_t p = {};
    const bool pressed = metalio_touch_read(&p);
    const bool blocked = OtaUpgradeScreen::IsActive() || StandbyScreen::IsActive();
    if (blocked || (tracking && start_screen != lv_screen_active())) {
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (consumed && !blocked) {
            // 松手后再创建遮罩，避免当前手势继续操作新面板。
            if (close_panel) {
                if (lv_async_call([](void*) { ScreenFrontlightHide(); }, nullptr) != LV_RESULT_OK)
                    ESP_LOGW(TAG, "quick panel close request failed");
            } else {
                if (lv_async_call([](void* screen) {
                    if (screen == lv_screen_active()) ScreenFrontlightShow();
                }, start_screen) != LV_RESULT_OK)
                    ESP_LOGW(TAG, "quick panel open request failed");
            }
        }
        tracking = false;
        consumed = false;
        ignore_until_release = false;
        return;
    }
    int ux = 0;
    int uy = 0;
    if (!MapLogicalToUi(p.x, p.y, &ux, &uy)) {
        data->state = LV_INDEV_STATE_RELEASED;
        tracking = false;
        consumed = false;
        ignore_until_release = true;
        return;
    }
    if (!blocked && !ignore_until_release && !tracking) {
        tracking = true;
        origin = {static_cast<int32_t>(ux), static_cast<int32_t>(uy)};
        start_screen = lv_screen_active();
        close_panel = ScreenFrontlightIsVisible();
        if ((close_panel && ScreenFrontlightOwnsDrag(ux, uy)) ||
            (!close_panel && PageOwnsVerticalDrag(origin))) {
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
    if (consumed) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = metalio_touch_has_pending_edges();
        return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = ux;
    data->point.y = uy;
    // 短按回放：同一次 indev 周期内立刻再读到 RELEASED，才能生成 CLICKED
    if (metalio_touch_has_pending_edges()) {
        data->continue_reading = true;
    }
}
