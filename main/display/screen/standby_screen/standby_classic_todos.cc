#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_todos.h"
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <lvgl.h>

#include "assets/lang_config.h"
#include "checklist_cache.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "task_screen/task_screen.h"

void StandbyClassic_FormatTodoPlanMeta(char* out, size_t out_sz, const TodoItem& item) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    char time_buf[8] = {};
    if (item.plan_time[0] != '\0') {
        std::snprintf(time_buf, sizeof(time_buf), "%.5s", item.plan_time);
    }
    if (item.plan_date[0] != '\0' && time_buf[0] != '\0') {
        std::snprintf(out, out_sz, "%s %s", item.plan_date, time_buf);
    } else if (item.plan_date[0] != '\0') {
        std::snprintf(out, out_sz, "%s", item.plan_date);
    } else if (time_buf[0] != '\0') {
        std::snprintf(out, out_sz, "%s", time_buf);
    } else {
        out[0] = '\0';
    }
}

void StandbyClassic_RebuildTodoList() {
    if (!StandbyClassic_alive || StandbyClassic_State().todo_host == nullptr) {
        return;
    }
    lv_obj_clean(StandbyClassic_State().todo_host);

    const lv_font_t* font = StandbyClassic_TodoFont();
    const lv_coord_t text_w = LV_HOR_RES - 2 * kSidePad - 2 * kTodoRowPad - 2 * kBorderW;

    if (StandbyClassic_todos.empty()) {
        lv_obj_t* empty = lv_label_create(StandbyClassic_State().todo_host);
        lv_label_set_text(empty, Lang::Strings::STANDBY_EMPTY_TODO);
        lv_obj_set_style_text_font(empty, font, 0);
        lv_obj_set_style_text_color(empty, lv_color_black(), 0);
        lv_obj_set_width(empty, text_w);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    const int n = static_cast<int>(StandbyClassic_todos.size()) < kMaxTodos ? static_cast<int>(StandbyClassic_todos.size())
                                                               : kMaxTodos;
    for (int i = 0; i < n; ++i) {
        const TodoItem& item = StandbyClassic_todos[static_cast<size_t>(i)];
        lv_obj_t* row = lv_obj_create(StandbyClassic_State().todo_host);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, kBorderW, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, kTodoRowPad, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (!StandbyClassic_as_overlay) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(row);
            lv_obj_add_event_cb(row, StandbyClassic_OnTodoItemClicked, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* text_col = lv_obj_create(row);
        lv_obj_remove_style_all(text_col);
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_set_height(text_col, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(text_col, kTodoLineGap, 0);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name = lv_label_create(text_col);
        lv_label_set_text(name, item.title);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, text_w);
        lv_obj_set_style_text_font(name, font, 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        char meta[72];
        StandbyClassic_FormatTodoPlanMeta(meta, sizeof(meta), item);
        if (meta[0] != '\0') {
            lv_obj_t* hint = lv_label_create(text_col);
            lv_label_set_text(hint, meta);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
            lv_obj_set_width(hint, text_w);
            lv_obj_set_style_text_font(hint, font, 0);
            lv_obj_set_style_text_color(hint, lv_color_black(), 0);
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void StandbyClassic_SetTodoHint(const char* text) {
    if (!StandbyClassic_alive || StandbyClassic_State().todo_host == nullptr) {
        return;
    }
    lv_obj_clean(StandbyClassic_State().todo_host);
    lv_obj_t* hint = lv_label_create(StandbyClassic_State().todo_host);
    lv_label_set_text(hint, text != nullptr ? text : "");
    lv_obj_set_style_text_font(hint, StandbyClassic_TodoFont(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, LV_HOR_RES - 2 * kSidePad - 2 * kTodoRowPad - 2 * kBorderW);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

void StandbyClassic_ApplyTodosFromCache() {
    StandbyClassic_todos.clear();
    if (!checklist_cache_ready()) {
        StandbyClassic_SetTodoHint(Lang::Strings::STANDBY_NO_TODO_CACHE);
        return;
    }
    checklist_cache_item_t buf[kMaxTodos];
    const size_t n = checklist_cache_copy_pending(buf, kMaxTodos);
    for (size_t i = 0; i < n; ++i) {
        TodoItem item;
        strlcpy(item.title, buf[i].title, sizeof(item.title));
        strlcpy(item.status, buf[i].status, sizeof(item.status));
        strlcpy(item.plan_date, buf[i].plan_date, sizeof(item.plan_date));
        strlcpy(item.plan_time, buf[i].plan_time, sizeof(item.plan_time));
        StandbyClassic_todos.push_back(item);
    }
    StandbyClassic_RebuildTodoList();
}

void StandbyClassic_OnHomeBtnClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "home btn -> home");
    lv_async_call(
        [](void*) {
            if (StandbyClassic_alive && StandbyClassic_as_overlay) {
                StandbyScreen::Dismiss();
                return;
            }
            ESP_LOGI(TAG, "return home");
            ScreenGoHome();
        },
        nullptr);
}

void StandbyClassic_OpenTaskScreenAsync(void* /*arg*/) {
    ScreenNavigateTo(TaskScreen::Create);
}

void StandbyClassic_OnTodoItemClicked(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "todo item -> task screen");
    lv_async_call(StandbyClassic_OpenTaskScreenAsync, nullptr);
}

