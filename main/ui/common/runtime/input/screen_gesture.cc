#include <cstdint>
#include <cstdlib>
#include <esp_log.h>
#include "infra/storage/key_bindings.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/runtime/input/screen_gesture.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/runtime/modal.h"

struct ScreenHorizontalSwipe {
    bool tracking = false; // 已记录本次触摸起点
    bool ignored = false; // 已交给控件或作废，松手前不再识别
    bool horizontal = false; // 已锁定横向拖动，不再交给纵向列表
    lv_dir_t direction = LV_DIR_NONE; // 已消费的滑动方向
    lv_point_t origin = {}; // 当前屏幕逻辑坐标
    lv_obj_t* screen = nullptr; // 手势开始时的页面，仅比较地址
    uint32_t epoch = 0; // 作废跨页面的手势
};

struct ScreenSwipeRequest {
    const char* key = nullptr; // 固定默认功能，不经过自定义按键绑定
    lv_obj_t* screen = nullptr; // 排队期间的页面身份
    uint32_t epoch = 0; // 排队期间的输入代次
};

static ScreenHorizontalSwipe s_swipe;
static ScreenSwipeRequest s_pending_swipe;

static lv_obj_t* FindGestureTarget(lv_obj_t* obj, lv_point_t point) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    lv_obj_transform_point(obj, &point, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2) {
        return nullptr;
    }
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_count(obj)) - 1; i >= 0; --i) {
        if (auto* child = FindGestureTarget(lv_obj_get_child(obj, i), point)) {
            return child;
        }
    }
    return obj;
}

static bool ControlOwnsHorizontalDrag(lv_point_t point) {
    lv_obj_t* layers[] = {lv_layer_sys(), lv_layer_top(), lv_screen_active()};
    for (auto* layer : layers) {
        auto* target = FindGestureTarget(layer, point);
        if (!target || target == layer) {
            continue;
        }
        for (auto* object = target; object; object = lv_obj_get_parent(object)) {
            if (lv_obj_check_type(object, &lv_slider_class) || lv_obj_check_type(object, &lv_switch_class) ||
                lv_obj_check_type(object, &lv_textarea_class) || lv_obj_check_type(object, &lv_keyboard_class)) {
                return true;
            }
            if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) &&
                (lv_obj_get_scroll_dir(object) & LV_DIR_HOR) != 0 &&
                lv_obj_get_scroll_left(object) + lv_obj_get_scroll_right(object) > 0) {
                return true;
            }
            if (UiModal::IsRoot(object)) {
                break;
            }
        }
        return false;
    }
    return false;
}

static bool SwipeNavigationAllowed() {
    return KeyBindingsGesturesEnabled() && !UiInputNavigationBlocked() && !ScreenFrontlightIsVisible();
}

static void DispatchHorizontalSwipe(void*) {
    const auto request = s_pending_swipe;
    s_pending_swipe = {};
    if (request.key && request.screen == lv_screen_active() && request.epoch == VkKey_InputEpoch() &&
        SwipeNavigationAllowed()) {
        VkKey_Dispatch(request.key);
    }
}

bool ScreenPageOwnsVerticalDrag(lv_point_t point) {
    lv_obj_t* layers[] = {lv_layer_sys(), lv_layer_top(), lv_screen_active()};
    for (auto* layer : layers) {
        auto* target = FindGestureTarget(layer, point);
        if (target == nullptr) {
            continue;
        }
        for (auto* obj = target; obj != nullptr; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, kTouchUiPagedVerticalDrag)) {
                return true;
            }
            const auto dir = lv_obj_get_scroll_dir(obj);
            // 仅实际存在纵向滚动范围的容器保留手势，长列表到达边界时仍优先滚动。
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && (dir & LV_DIR_VER) != 0 &&
                lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj) > 0) {
                return true;
            }
            if (UiModal::IsRoot(obj)) {
                break;
            }
        }
        if (target != layer) {
            return false;
        }
    }
    return false;
}

bool ScreenHandleHorizontalSwipe(lv_indev_t* indev, bool pressed, lv_point_t& point,
                                 lv_point_t origin, bool valid) {
    const bool consumed = s_swipe.horizontal;
    if (!pressed) {
        if (s_swipe.direction != LV_DIR_NONE && !s_swipe.ignored && s_swipe.screen == lv_screen_active() &&
            s_swipe.epoch == VkKey_InputEpoch() && SwipeNavigationAllowed()) {
            s_pending_swipe = {s_swipe.direction == LV_DIR_LEFT ? "vk_next" : "vk_prev",
                               s_swipe.screen, s_swipe.epoch};
            if (lv_async_call(DispatchHorizontalSwipe, nullptr) != LV_RESULT_OK) {
                s_pending_swipe = {};
                ESP_LOGE("ScreenGesture", "operation=page_swipe stage=dispatch result=queue_failed");
            }
        }
        s_swipe = {};
        return consumed;
    }
    if (s_swipe.ignored) {
        return consumed;
    }
    if (!valid || !SwipeNavigationAllowed() || s_pending_swipe.key ||
        (s_swipe.tracking && (s_swipe.screen != lv_screen_active() || s_swipe.epoch != VkKey_InputEpoch()))) {
        s_swipe.ignored = true;
        return consumed;
    }
    if (!s_swipe.tracking) {
        s_swipe.tracking = true;
        s_swipe.origin = origin;
        s_swipe.screen = lv_screen_active();
        s_swipe.epoch = VkKey_InputEpoch();
        if (ControlOwnsHorizontalDrag(origin)) {
            s_swipe.ignored = true;
            return false;
        }
    }
    const int dx = point.x - s_swipe.origin.x;
    const int dy = point.y - s_swipe.origin.y;
    const int distance_x = std::abs(dx);
    const int distance_y = std::abs(dy);
    if (!s_swipe.horizontal) {
        const bool horizontal = distance_x >= UiSx(16) && distance_x * 2 > distance_y * 3;
        const bool vertical = distance_y >= UiSy(16) && distance_y * 2 > distance_x * 3;
        const bool diagonal = distance_x >= UiSx(32) || distance_y >= UiSy(32);
        if (vertical || (diagonal && !horizontal && distance_y >= distance_x)) {
            s_swipe.ignored = true;
            return false;
        }
        if (!horizontal && !diagonal) {
            // 方向尚未明确时固定触点，避免起手抖动先触发 LVGL 纵向滚动。
            point = s_swipe.origin;
            return false;
        }
        s_swipe.horizontal = true;
        lv_indev_wait_release(indev);
    }
    if (s_swipe.direction == LV_DIR_NONE && distance_x >= UiSx(60)) {
        s_swipe.direction = dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT;
    }
    return true;
}
