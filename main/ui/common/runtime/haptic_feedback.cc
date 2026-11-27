#include <atomic>
#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "haptic_feedback.h"

static constexpr const char* TAG = "Haptic";
// LVGL USER_1：标记已挂震动回调，避免重复 Attach 导致连震。
static constexpr lv_obj_flag_t kHapticBoundFlag = LV_OBJ_FLAG_USER_1;
static constexpr size_t kMaxZones = 96;

struct HapticFeedbackHapticZone {
    lv_obj_t* obj = nullptr;
    bool active = false;
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
};

static std::atomic<bool> s_zone_early_pulse{true};
// touch_feed 早震与 LVGL PRESSED 去重
static uint32_t s_press_seq = 0;
static uint32_t s_pulsed_seq = 0;

static HapticFeedbackHapticZone s_zones[kMaxZones]{};
static size_t s_zone_count = 0;
static portMUX_TYPE s_zone_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ReadZoneArea(lv_obj_t* obj, lv_area_t* area) {
    lv_obj_get_coords(obj, area);
    if (!lv_obj_area_is_visible(obj, area)) {
        return false;
    }
    for (lv_obj_t* node = obj; node != nullptr; node = lv_obj_get_parent(node)) {
        if (lv_obj_has_state(node, LV_STATE_DISABLED)) {
            return false;
        }
    }
    return true;
}

static void WriteZoneCoords(HapticFeedbackHapticZone* z, lv_obj_t* obj) {
    lv_area_t a{};
    const bool active = ReadZoneArea(obj, &a);
    if (z->obj == obj && z->active == active && z->x1 == a.x1 && z->y1 == a.y1 &&
        z->x2 == a.x2 && z->y2 == a.y2) {
        return;
    }
    portENTER_CRITICAL(&s_zone_mux);
    z->obj = obj;
    z->active = active;
    z->x1 = a.x1;
    z->y1 = a.y1;
    z->x2 = a.x2;
    z->y2 = a.y2;
    portEXIT_CRITICAL(&s_zone_mux);
}

static HapticFeedbackHapticZone* FindZoneByObj(const lv_obj_t* obj) {
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            return &s_zones[i];
        }
    }
    return nullptr;
}

static void RemoveZoneAt(size_t idx) {
    if (idx >= s_zone_count) {
        return;
    }
    portENTER_CRITICAL(&s_zone_mux);
    s_zones[idx] = s_zones[s_zone_count - 1];
    s_zones[s_zone_count - 1] = {};
    s_zone_count--;
    portEXIT_CRITICAL(&s_zone_mux);
}

static bool RegisterZone(lv_obj_t* obj) {
    HapticFeedbackHapticZone* existing = FindZoneByObj(obj);
    if (existing != nullptr) {
        WriteZoneCoords(existing, obj);
        return true;
    }
    if (s_zone_count >= kMaxZones) {
        // Long font/TOC lists contain many off-screen buttons. Only retain
        // visible early-feedback zones; PRESSED feedback still works for all.
        for (size_t i = s_zone_count; i > 0; --i) {
            if (!lv_obj_is_visible(s_zones[i - 1].obj)) {
                RemoveZoneAt(i - 1);
            }
        }
        if (s_zone_count >= kMaxZones) {
            static bool warned = false;
            if (!warned) {
                ESP_LOGW(TAG, "visible zone table full (%u); use press feedback", static_cast<unsigned>(kMaxZones));
                warned = true;
            }
            return false;
        }
    }
    lv_area_t a{};
    const bool active = ReadZoneArea(obj, &a);
    portENTER_CRITICAL(&s_zone_mux);
    HapticFeedbackHapticZone* z = &s_zones[s_zone_count];
    z->obj = obj;
    z->active = active;
    z->x1 = a.x1;
    z->y1 = a.y1;
    z->x2 = a.x2;
    z->y2 = a.y2;
    s_zone_count++;
    portEXIT_CRITICAL(&s_zone_mux);
    return true;
}

static void OnGeomChanged(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    HapticFeedbackHapticZone* z = FindZoneByObj(obj);
    if (z == nullptr) {
        if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN_BEGIN && lv_obj_is_visible(obj)) {
            RegisterZone(obj);
        }
        return;
    }
    // 显隐或禁用状态变化也必须刷新热区，不能只比较坐标。
    WriteZoneCoords(z, obj);
}

static void OnObjDeleted(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            RemoveZoneAt(i);
            return;
        }
    }
}

static void OnPressFeedback(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    HapticFeedbackHapticZone* z = FindZoneByObj(obj);
    if (z != nullptr) {
        WriteZoneCoords(z, obj);
    }
    portENTER_CRITICAL(&s_zone_mux);
    if (s_press_seq != 0 && s_pulsed_seq == s_press_seq) {
        portEXIT_CRITICAL(&s_zone_mux);
        return;
    }
    // 占位，防止同一次按下多次 PRESSED / 抬手后晚到的 PRESSED 再震
    if (s_press_seq != 0) {
        s_pulsed_seq = s_press_seq;
    }
    portEXIT_CRITICAL(&s_zone_mux);
    HapticPulseIfEnabled();
}

void HapticBeginPress(void) {
    portENTER_CRITICAL(&s_zone_mux);
    if (++s_press_seq == 0) {
        ++s_press_seq;
    }
    s_pulsed_seq = 0;
    portEXIT_CRITICAL(&s_zone_mux);
}

void HapticPulseOnFingerDown(void) {
    portENTER_CRITICAL(&s_zone_mux);
    if (s_press_seq == 0) {
        s_press_seq = 1;
    }
    const bool pulse = s_pulsed_seq != s_press_seq;
    s_pulsed_seq = s_press_seq;
    portEXIT_CRITICAL(&s_zone_mux);
    if (pulse) {
        HapticPulseIfEnabled();
    }
}

bool HapticTryPulseAtUiPoint(int16_t x, int16_t y) {
    if (!HapticIsEnabled() || !s_zone_early_pulse) {
        return false;
    }
    bool hit = false;
    portENTER_CRITICAL(&s_zone_mux);
    for (size_t i = 0; i < s_zone_count; ++i) {
        const HapticFeedbackHapticZone& z = s_zones[i];
        if (z.obj == nullptr || !z.active) {
            continue;
        }
        if (x >= z.x1 && x <= z.x2 && y >= z.y1 && y <= z.y2) {
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_zone_mux);
    if (!hit) {
        return false;
    }
    HapticPulseOnFingerDown();
    return true;
}

void HapticSetZoneEarlyPulseEnabled(bool enabled) {
    s_zone_early_pulse = enabled;
}

void HapticRefreshZones(void) {
    for (size_t i = 0; i < s_zone_count; ++i) {
        WriteZoneCoords(&s_zones[i], s_zones[i].obj);
    }
}

void HapticAttachClick(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    if (lv_obj_has_flag(obj, kHapticBoundFlag)) {
        return;
    }
    lv_obj_add_flag(obj, kHapticBoundFlag);
    lv_obj_add_event_cb(obj, OnPressFeedback, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, OnGeomChanged, LV_EVENT_SIZE_CHANGED, nullptr);
    // 布局常改位置不改尺寸：SIZE_CHANGED 不来，上屏绘制时刷新热区坐标
    lv_obj_add_event_cb(obj, OnGeomChanged, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(obj, OnObjDeleted, LV_EVENT_DELETE, nullptr);
    // Register after layout, when DRAW supplies the actual visible coordinates.
}

void HapticDetachClick(lv_obj_t* obj) {
    if (obj == nullptr || !lv_obj_has_flag(obj, kHapticBoundFlag)) {
        return;
    }
    lv_obj_clear_flag(obj, kHapticBoundFlag);
    lv_obj_remove_event_cb(obj, OnPressFeedback);
    lv_obj_remove_event_cb(obj, OnGeomChanged);
    lv_obj_remove_event_cb(obj, OnObjDeleted);
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            RemoveZoneAt(i);
            break;
        }
    }
}
