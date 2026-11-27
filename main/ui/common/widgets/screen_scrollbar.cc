#include <esp_log.h>
#include "ui/common/widgets/screen_scrollbar.h"

static constexpr uint32_t kScrollbarVisibleMs = 3000;
static constexpr uint32_t kScrollbarPollMs = 200;

struct ScreenScrollbarState {
    lv_obj_t* viewport; // 生命周期由控件删除事件结束
    ScreenScrollbarState* next; // UI 任务维护的状态链
    uint32_t shown_at; // 最近一次显示的 LVGL 时刻
    lv_opa_t opacity; // 原生滚动条显示时保留的透明度
    bool custom; // 控件自行绘制滚动条
    bool visible; // 当前是否允许显示
    bool active; // 上次检查时位于可见界面
    bool seen; // 本次检查已遍历到
};

static ScreenScrollbarState* s_scrollbars = nullptr;
static uint32_t s_last_poll = 0;
static bool s_allocation_warned = false;

static ScreenScrollbarState* FindScrollbar(lv_obj_t* viewport) {
    for (auto* state = s_scrollbars; state; state = state->next) {
        if (state->viewport == viewport) {
            return state;
        }
    }
    return nullptr;
}

static void SetScrollbarVisible(ScreenScrollbarState* state, bool visible) {
    if (state->visible == visible) {
        return;
    }
    state->visible = visible;
    if (state->custom) {
        lv_obj_invalidate(state->viewport);
    } else {
        lv_obj_set_style_opa(state->viewport, visible ? state->opacity : static_cast<lv_opa_t>(LV_OPA_TRANSP),
                             LV_PART_SCROLLBAR);
    }
}

static void ScrollbarEvent(lv_event_t* event) {
    auto* state = static_cast<ScreenScrollbarState*>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_GESTURE && lv_event_get_target_obj(event) != state->viewport) {
        return;
    }
    if (code == LV_EVENT_DELETE) {
        auto** link = &s_scrollbars;
        while (*link && *link != state) {
            link = &(*link)->next;
        }
        if (*link) {
            *link = state->next;
        }
        lv_obj_remove_event_cb_with_user_data(state->viewport, ScrollbarEvent, state);
        lv_free(state);
    } else if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN ||
               code == LV_EVENT_SCROLL_END || code == LV_EVENT_CHILD_CHANGED || code == LV_EVENT_GESTURE) {
        state->shown_at = lv_tick_get();
        SetScrollbarVisible(state, true);
    }
}

static ScreenScrollbarState* RegisterScrollbar(lv_obj_t* viewport, bool custom) {
    if (auto* existing = FindScrollbar(viewport)) {
        existing->custom = existing->custom || custom;
        return existing;
    }
    // 项目的 LVGL 分配器使用 PSRAM，不挤占内部 RAM 安全余量。
    auto* state = static_cast<ScreenScrollbarState*>(lv_malloc(sizeof(ScreenScrollbarState)));
    if (!state) {
        if (!s_allocation_warned) {
            ESP_LOGW("Scrollbar", "operation=register result=no_memory fallback=visible");
            s_allocation_warned = true;
        }
        return nullptr;
    }
    *state = {viewport, s_scrollbars, lv_tick_get(), lv_obj_get_style_opa(viewport, LV_PART_SCROLLBAR),
              custom, true, false, false};
    if (!lv_obj_add_event_cb(viewport, ScrollbarEvent, LV_EVENT_ALL, state)) {
        lv_free(state);
        if (!s_allocation_warned) {
            ESP_LOGW("Scrollbar", "operation=register stage=events result=no_memory fallback=visible");
            s_allocation_warned = true;
        }
        return nullptr;
    }
    s_allocation_warned = false;
    s_scrollbars = state;
    if (!custom && lv_obj_get_scrollbar_mode(viewport) == LV_SCROLLBAR_MODE_ACTIVE) {
        lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
    }
    return state;
}

static void DiscoverScrollbars(lv_obj_t* object) {
    if (!object || !lv_obj_is_visible(object)) {
        return;
    }
    auto* state = FindScrollbar(object);
    if (!state && lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) &&
        lv_obj_get_scrollbar_mode(object) != LV_SCROLLBAR_MODE_OFF &&
        (lv_obj_get_scroll_top(object) + lv_obj_get_scroll_bottom(object) > 0 ||
         lv_obj_get_scroll_left(object) + lv_obj_get_scroll_right(object) > 0)) {
        state = RegisterScrollbar(object, false);
    }
    if (state) {
        state->seen = true;
        if (!state->active) {
            state->shown_at = lv_tick_get();
            SetScrollbarVisible(state, true);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        DiscoverScrollbars(lv_obj_get_child(object, i));
    }
}

void ScreenScrollbarAttach(lv_obj_t* viewport) {
    if (viewport) {
        RegisterScrollbar(viewport, true);
        ScreenScrollbarShow(viewport);
    }
}

void ScreenScrollbarShow(lv_obj_t* viewport) {
    if (auto* state = FindScrollbar(viewport)) {
        state->shown_at = lv_tick_get();
        SetScrollbarVisible(state, true);
    }
}

bool ScreenScrollbarVisible(lv_obj_t* viewport) {
    auto* state = FindScrollbar(viewport);
    return !state || state->visible;
}

void ScreenScrollbarPoll() {
    const uint32_t now = lv_tick_get();
    if (now - s_last_poll < kScrollbarPollMs) {
        return;
    }
    s_last_poll = now;
    for (auto* state = s_scrollbars; state; state = state->next) {
        state->seen = false;
    }
    DiscoverScrollbars(lv_screen_active());
    DiscoverScrollbars(lv_layer_top());
    DiscoverScrollbars(lv_layer_sys());
    for (auto* state = s_scrollbars; state; state = state->next) {
        state->active = state->seen;
        if (state->active && state->visible && now - state->shown_at >= kScrollbarVisibleMs &&
            !lv_obj_has_state(state->viewport, LV_STATE_SCROLLED)) {
            SetScrollbarVisible(state, false);
        }
    }
}
