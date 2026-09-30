/**
 * @file lv_adapter_epdiy_touch.cc
 * @brief LVGL 触摸：盖板 VK + 屏内早震（独立 TU，避免 epdiy 大文件触发 GCC ICE）
 */

#include "lv_adapter_epdiy_touch.h"

#include "config.h"
#include "haptic_feedback.h"
#include "metalio_touch.h"
#include "power_policy.h"
#include "vk_key_handler.h"

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "epdiy_touch"

/** @brief 逻辑坐标 → UI：扣 content inset 再加板级微调；越界返回 false */
static bool MapLogicalToUi(int lx, int ly, int* ux, int* uy) {
    const int x = lx - DISPLAY_CONTENT_LEFT_INSET + DISPLAY_TOUCH_OFFSET_X;
    const int y = ly - DISPLAY_CONTENT_TOP_INSET + DISPLAY_TOUCH_OFFSET_Y;
    if (x < 0 || x >= DISPLAY_CONTENT_W || y < 0 || y >= DISPLAY_CONTENT_H) {
        return false;
    }
    *ux = x;
    *uy = y;
    return true;
}

static const char* s_vk_hold_name = nullptr;
static int64_t s_vk_hold_down_us = 0;
static bool s_vk_hold_long = false;
static bool s_vk_hold_long_consumed = false;
static bool s_ui_finger_down = false;

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
        const bool emit_click = !s_vk_hold_long_consumed;
        VkKey_OnPressUp(name);
        if (emit_click) {
            HapticPulseIfEnabled();
            VkKey_Dispatch(name);
        }
    }
    s_vk_hold_name = nullptr;
    s_vk_hold_down_us = 0;
    s_vk_hold_long = false;
    s_vk_hold_long_consumed = false;
}

static void BeginCoverHold(const char* vk, int raw_x, int raw_y) {
    s_vk_hold_name = vk;
    s_vk_hold_down_us = esp_timer_get_time();
    s_vk_hold_long = false;
    s_vk_hold_long_consumed = false;
    HapticBeginPress();
    ESP_LOGI(TAG, "cover key [%s] down raw=(%d,%d)", vk, raw_x, raw_y);
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
                ESP_LOGI(TAG, "cover key [%s] long-press", s_vk_hold_name);
                s_vk_hold_long_consumed = VkKey_OnLongPress(s_vk_hold_name);
                if (s_vk_hold_long_consumed) {
                    HapticPulseIfEnabled();
                }
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
        ESP_LOGI(TAG, "touch down ui=%d,%d", ux, uy);
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

void LvAdapterEpdiy_TouchReadCb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    metalio_touch_point_t p = {};
    if (!metalio_touch_read(&p)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int ux = 0;
    int uy = 0;
    if (!MapLogicalToUi(p.x, p.y, &ux, &uy)) {
        data->state = LV_INDEV_STATE_RELEASED;
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
