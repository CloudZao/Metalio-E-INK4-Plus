#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>
#include <esp_log.h>
#include <esp_timer.h>
#include "business/app_store/app_data_store.h"
#include "business/app_store/app_network.h"
#include "ui/apps/app_store/app_runtime_services.h"
#include "boards/common/board/board.h"
#include "boards/common/haptics/vibration.h"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "boards/common/storage/sd_access.h"
#include "business/app_store/app_audio_player.h"
#include "business/app_store/app_audio_recorder.h"
#include "business/app_store/app_camera_preview.h"
#include "business/haptics/haptic_preferences.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/apps/app_store/app_runtime_images.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_keyboard.h"
#include "ui/apps/standby/standby_screen.h"
#include "sysmgmt/system_state.h"
#include "ui/common/drawing/ui_theme.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S31
// GCC 15.2 编译运行期服务时在 IPA modref 崩溃，仅关闭该项分析。
#pragma GCC optimize ("no-ipa-modref")
#endif

static constexpr const char* TAG = "AppServices";

static AppRuntimeServicesState* Active(void* host) {
    auto* state = static_cast<AppRuntimeServicesState*>(host);
    return state && state->open ? state : nullptr;
}

static int LocalTime(xz_app_time_t* out) {
    if (!out) {
        return XZ_APP_ERR_INVALID;
    }
    *out = {};
    const time_t now = std::time(nullptr);
    struct tm date{};
    if (!localtime_r(&now, &date) || date.tm_year < 120) {
        return XZ_APP_ERR_NOT_READY;
    }
    *out = {static_cast<int64_t>(now),
            static_cast<int16_t>(date.tm_year + 1900),
            static_cast<uint8_t>(date.tm_mon + 1),
            static_cast<uint8_t>(date.tm_mday),
            static_cast<uint8_t>(date.tm_hour),
            static_cast<uint8_t>(date.tm_min),
            static_cast<uint8_t>(date.tm_sec),
            static_cast<uint8_t>(date.tm_wday)};
    return XZ_APP_OK;
}

static uint64_t MonotonicMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

static bool AppUiSuspended() {
    const auto phase = SystemState::GetInstance().Phase();
    return phase == SystemPhase::Updating || phase == SystemPhase::Restarting || StandbyPresentation::IsActive() ||
           StandbyPresentation::IsResuming();
}

static AppRuntimeServicesState* ActiveNetwork(void* host) {
    auto* state = Active(host);
    if (!state || state->closing || !state->foreground_timer || AppUiSuspended() || state->transition_pending ||
        lv_obj_get_screen(state->root) != lv_screen_active()) {
        return nullptr;
    }
    return state;
}

static void ReleaseAppVibration(AppRuntimeServicesState& state) {
    Vibration_CancelPulse(state.vibration_id);
    state.vibration_id = 0;
    state.vibration_until = 0;
    state.vibration_hold.reset();
}

static void UpdateForeground(AppRuntimeServicesState& state) {
    const uint64_t now = MonotonicMs() - StandbyPresentation::TotalDurationMs();
    if (state.full_refresh_pending &&
        static_cast<int32_t>(EpdGetStats().completed - state.full_refresh_sequence) > 0) {
        state.full_refresh_pending = false;
    }
    if (state.transition_pending) {
        const auto stats = EpdGetStats();
        if (static_cast<int32_t>(stats.submitted - state.transition_sequence) > 0 &&
            static_cast<int32_t>(stats.completed - state.transition_sequence) > 0) {
            state.transition_pending = false;
        }
    }
    const bool foreground = !state.closing && !state.transition_pending && !AppUiSuspended() &&
                            lv_obj_get_screen(state.root) == lv_screen_active();
    if (foreground && state.foreground) {
        state.foreground_elapsed += now - state.foreground_last;
    }
    state.foreground_last = now;
    state.foreground = foreground;
    if (!foreground) {
        AppNetworkStop(state.network, false);
    } else {
        AppNetworkForeground(state.network, true);
    }
    int image_width = 0, image_height = 0;
    if (state.camera_widget && !AppImagesSize(state.images, state.camera_widget, image_width, image_height)) {
        AppCameraPreviewEnable(state.camera, false, 0, 0);
        state.camera_widget = nullptr;
    }
    AppCameraPreviewForeground(state.camera, foreground && state.camera_widget);
    if (foreground) {
        AppImagesPoll(state.images);
        if (state.camera_widget) {
            AppImagesPresent(state.images, state.camera_widget, AppCameraPreviewTake(state.camera));
        }
    }
    AppAudioSetForeground(state.audio, foreground);
    AppRecordForeground(state.recorder, foreground);
    // 休眠会清除驱动订阅，恢复前台时重申请求，不能只依赖宿主缓存的状态边沿。
    const bool sampling = foreground && state.acceleration_requested;
    if (sampling || state.acceleration_running) {
        Accelerometer_SetAppTracking(sampling);
    }
    state.acceleration_running = sampling;
    if (state.vibration_until && (!foreground || MonotonicMs() >= state.vibration_until)) {
        ReleaseAppVibration(state);
    }
}

static void TimerTick(lv_timer_t* handle) {
    auto* timer = static_cast<AppRuntimeServicesTimer*>(lv_timer_get_user_data(handle));
    auto* state = static_cast<AppRuntimeServicesState*>(timer->state);
    if (state && state->open) {
        UpdateForeground(*state);
    }
    if (AppUiSuspended() || (state && state->transition_pending) ||
        lv_obj_get_screen(timer->root) != lv_screen_active()) {
        return;
    }
    const auto callback = timer->callback;
    void* context = timer->context;
    if (!timer->repeat) {
        lv_timer_delete(timer->handle);
        *timer = {};
    }
    callback(context);
}

static uint32_t StartTimer(void* host, uint32_t interval, int repeat, xz_app_click_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing || !callback || interval < 200 || interval > 86400000) {
        return 0;
    }
    auto slot = std::find_if(state->timers.begin(), state->timers.end(),
                             [](const AppRuntimeServicesTimer& value) { return !value.handle; });
    if (slot == state->timers.end() || state->next_id == UINT32_MAX) {
        return 0;
    }
    slot->handle = lv_timer_create(TimerTick, interval, &*slot);
    if (!slot->handle) {
        ESP_LOGE(TAG, "operation=timer_start stage=allocate result=failed");
        return 0;
    }
    slot->id = ++state->next_id;
    slot->repeat = repeat != 0;
    slot->callback = callback;
    slot->context = context;
    slot->root = state->root;
    slot->state = state;
    ESP_LOGD(TAG, "operation=timer_start result=success id=%lu interval_ms=%lu", (unsigned long)slot->id,
             (unsigned long)interval);
    return slot->id;
}

static int CancelTimer(void* host, uint32_t id) {
    auto* state = Active(host);
    if (!state) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!id) {
        return XZ_APP_ERR_INVALID;
    }
    for (auto& timer : state->timers) {
        if (timer.handle && timer.id == id) {
            lv_timer_delete(timer.handle);
            timer = {};
            return XZ_APP_OK;
        }
    }
    return XZ_APP_ERR_NOT_FOUND;
}

static int ReadData(void* host, const char* key, void* buffer, uint32_t capacity) {
    auto* state = Active(host);
    return state ? AppReadData(state->storage_key, key, buffer, capacity) : XZ_APP_ERR_NOT_READY;
}

static int WriteData(void* host, const char* key, const void* data, uint32_t length) {
    auto* state = Active(host);
    return state ? AppWriteData(state->storage_key, key, data, length) : XZ_APP_ERR_NOT_READY;
}

static int RemoveData(void* host, const char* key) {
    auto* state = Active(host);
    return state ? AppRemoveData(state->storage_key, key) : XZ_APP_ERR_NOT_READY;
}

static void* Container(void* parent, int x, int y, int width, int height, int scroll_y) {
    if (!parent) {
        return nullptr;
    }
    auto* object = lv_obj_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    if (scroll_y) {
        lv_obj_set_scroll_dir(object, LV_DIR_VER);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    }
    return object;
}

struct AppRuntimeServicesInputCallback {
    xz_app_click_t callback;  // 应用输入确认回调
    void* context;            // 应用确认数据
};

static void* Input(void* parent, int x, int y, int width, int height, const char* text, uint32_t max_chars,
                   xz_app_click_t submit, void* context) {
    if (!parent || !max_chars || max_chars > 256) {
        return nullptr;
    }
    auto* binding = submit ? new (std::nothrow) AppRuntimeServicesInputCallback{submit, context} : nullptr;
    if (submit && !binding) {
        ESP_LOGE(TAG, "operation=input stage=allocate result=failed");
        return nullptr;
    }
    auto* object = lv_textarea_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_textarea_set_one_line(object, true);
    lv_textarea_set_max_length(object, max_chars);
    lv_textarea_set_text(object, text ? text : "");
    if (binding) {
        lv_obj_add_event_cb(
            object,
            [](lv_event_t* event) {
                auto* data = static_cast<AppRuntimeServicesInputCallback*>(lv_event_get_user_data(event));
                if (lv_event_get_code(event) == LV_EVENT_DELETE) {
                    delete data;
                } else if (lv_event_get_code(event) == LV_EVENT_READY) {
                    data->callback(data->context);
                }
            },
            LV_EVENT_ALL, binding);
    }
    return object;
}

static int GetText(void* widget, char* buffer, uint32_t capacity) {
    if (!widget) {
        return XZ_APP_ERR_INVALID;
    }
    auto* object = static_cast<lv_obj_t*>(widget);
    if (lv_obj_check_type(object, &lv_button_class)) {
        object = lv_obj_get_child(object, 0);
    }
    const char* value = nullptr;
    if (object && lv_obj_check_type(object, &lv_label_class)) {
        value = lv_label_get_text(object);
    } else if (object && lv_obj_check_type(object, &lv_textarea_class)) {
        value = lv_textarea_get_text(object);
    }
    if (!value) {
        return XZ_APP_ERR_INVALID;
    }
    const size_t length = strlen(value);
    if (!buffer) {
        return static_cast<int>(length);
    }
    if (capacity <= length) {
        if (capacity) {
            buffer[0] = 0;
        }
        return XZ_APP_ERR_LIMIT;
    }
    memcpy(buffer, value, length + 1);
    return static_cast<int>(length);
}

struct AppRuntimeServicesKeyboardBinding {
    AppRuntimeServicesState* state;  // 键盘所属运行环境
    lv_obj_t* input;                 // 输入框被删除时置空
};

static void InputDeleted(lv_event_t* event) {
    static_cast<AppRuntimeServicesKeyboardBinding*>(lv_event_get_user_data(event))->input = nullptr;
}

static void* Keyboard(void* host, void* parent, void* input) {
    auto* state = Active(host);
    auto* target = static_cast<lv_obj_t*>(input);
    if (!state || state->closing || !parent || !target || state->keyboard ||
        !lv_obj_check_type(target, &lv_textarea_class)) {
        return nullptr;
    }
    auto* binding = new (std::nothrow) AppRuntimeServicesKeyboardBinding{state, target};
    if (!binding) {
        ESP_LOGE(TAG, "operation=keyboard stage=allocate result=failed");
        return nullptr;
    }
    auto* keyboard = ScreenKeyboardCreate(
        static_cast<lv_obj_t*>(parent),
        [](ScreenKeyboardEvent event, const char* text, void* context) {
            auto* data = static_cast<AppRuntimeServicesKeyboardBinding*>(context);
            if (!data->input || !data->state->open || data->state->closing || AppUiSuspended() ||
                lv_obj_get_screen(data->state->root) != lv_screen_active()) {
                return;
            }
            if (event == ScreenKeyboardEvent::kInsert) {
                lv_textarea_add_text(data->input, text);
            } else if (event == ScreenKeyboardEvent::kBackspace) {
                lv_textarea_delete_char(data->input);
            } else if (event == ScreenKeyboardEvent::kClear) {
                lv_textarea_set_text(data->input, "");
            } else if (event == ScreenKeyboardEvent::kConfirm) {
                lv_obj_send_event(data->input, LV_EVENT_READY, nullptr);
            }
        },
        binding);
    if (!keyboard) {
        delete binding;
        ESP_LOGE(TAG, "operation=keyboard stage=create result=failed");
        return nullptr;
    }
    state->keyboard = keyboard;
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(target, InputDeleted, LV_EVENT_DELETE, binding);
    lv_obj_add_event_cb(
        keyboard,
        [](lv_event_t* event) {
            auto* data = static_cast<AppRuntimeServicesKeyboardBinding*>(lv_event_get_user_data(event));
            if (data->input) {
                lv_obj_remove_event_cb_with_user_data(data->input, InputDeleted, data);
            }
            data->state->keyboard = nullptr;
            delete data;
        },
        LV_EVENT_DELETE, binding);
    ScreenKeyboardReset(keyboard);
    lv_obj_update_layout(keyboard);
    lv_obj_move_foreground(keyboard);
    ESP_LOGI(TAG, "operation=keyboard result=opened");
    return keyboard;
}

static int CloseKeyboard(void* host) {
    auto* state = Active(host);
    if (!state) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!state->keyboard) {
        return XZ_APP_ERR_NOT_FOUND;
    }
    ScreenKeyboardPause(state->keyboard);
    lv_obj_delete(state->keyboard);
    return XZ_APP_OK;
}

static void* Progress(void* parent, int x, int y, int width, int height) {
    if (!parent) {
        return nullptr;
    }
    auto* object = lv_bar_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_bar_set_range(object, 0, 100);
    lv_obj_set_style_bg_color(object, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(object, UiColorForeground(), LV_PART_INDICATOR);
    return object;
}

static void SetProgress(void* widget, uint32_t percent) {
    auto* object = static_cast<lv_obj_t*>(widget);
    if (object && lv_obj_check_type(object, &lv_bar_class)) {
        lv_bar_set_value(object, std::min<uint32_t>(percent, 100), LV_ANIM_OFF);
    }
}

static int OnExit(void* host, xz_app_click_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    state->on_exit = callback;
    state->exit_context = context;
    return XZ_APP_OK;
}

static int KeepAwake(void* host, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (enabled && !state->awake) {
        state->awake.emplace(PowerNeed::StandbyInhibit);
    } else if (!enabled) {
        state->awake.reset();
    }
    return XZ_APP_OK;
}

static void Notify(void* host, const char* text) {
    auto* state = Active(host);
    if (state && !state->closing && !AppUiSuspended() &&
        lv_obj_get_screen(state->root) == lv_screen_active()) {
        ScreenShowToast(text);
    }
}

static void ClearChildren(void* parent) {
    auto* object = static_cast<lv_obj_t*>(parent);
    if (object) {
        lv_obj_clean(object);
    }
}

struct AppRuntimeServicesDrawing {
    std::vector<xz_app_draw_t> commands;  // 已提交的绘图命令，主绘制事件复用
};

static void DrawingEvent(lv_event_t* event) {
    auto* drawing = static_cast<AppRuntimeServicesDrawing*>(lv_event_get_user_data(event));
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        delete drawing;
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) {
        return;
    }
    // 每次主绘制都按缓存命令重画，叠加控件或推白后重绘仍能恢复完整画面。
    auto* object = static_cast<lv_obj_t*>(lv_event_get_target(event));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    auto* layer = lv_event_get_layer(event);
    for (const auto& command : drawing->commands) {
        if (command.kind == 0) {
            lv_draw_line_dsc_t line;
            lv_draw_line_dsc_init(&line);
            line.color = lv_color_black();
            line.width = command.width;
            line.round_start = true;
            line.round_end = true;
            line.p1.x = area.x1 + command.x1;
            line.p1.y = area.y1 + command.y1;
            line.p2.x = area.x1 + command.x2;
            line.p2.y = area.y1 + command.y2;
            lv_draw_line(layer, &line);
        } else {
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.radius = command.kind == 2 ? LV_RADIUS_CIRCLE : 0;
            rect.border_color = lv_color_black();
            rect.border_width = command.width;
            rect.bg_color = lv_color_black();
            rect.bg_opa = command.filled ? LV_OPA_COVER : LV_OPA_TRANSP;
            const lv_area_t bounds{area.x1 + command.x1, area.y1 + command.y1,
                                   area.x1 + command.x2, area.y1 + command.y2};
            lv_draw_rect(layer, &rect, &bounds);
        }
    }
}

static void* Drawing(void* parent, int x, int y, int width, int height) {
    if (!parent || width < 1 || height < 1 || width > 1024 || height > 1600) {
        return nullptr;
    }
    auto* drawing = new (std::nothrow) AppRuntimeServicesDrawing;
    if (!drawing) {
        ESP_LOGE(TAG, "operation=drawing stage=allocate result=failed");
        return nullptr;
    }
    auto* object = lv_obj_create(static_cast<lv_obj_t*>(parent));
    if (!object) {
        delete drawing;
        ESP_LOGE(TAG, "operation=drawing stage=create result=failed");
        return nullptr;
    }
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(object, drawing);
    lv_obj_add_event_cb(object, DrawingEvent, LV_EVENT_ALL, drawing);
    return object;
}

static AppRuntimeServicesDrawing* DrawingOf(void* widget) {
    auto* object = static_cast<lv_obj_t*>(widget);
    if (!object) {
        return nullptr;
    }
    for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
        auto* dsc = lv_obj_get_event_dsc(object, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == DrawingEvent) {
            return static_cast<AppRuntimeServicesDrawing*>(lv_event_dsc_get_user_data(dsc));
        }
    }
    return nullptr;
}

static int Draw(void* widget, const xz_app_draw_t* commands, uint32_t count) {
    auto* drawing = DrawingOf(widget);
    if (!drawing || (!commands && count) || count > 1024) {
        return XZ_APP_ERR_INVALID;
    }
    auto* object = static_cast<lv_obj_t*>(widget);
    // LVGL 延迟计算坐标；首次提交绘图前必须落实创建时设置的尺寸。
    lv_obj_update_layout(object);
    const int area_w = lv_obj_get_width(object);
    const int area_h = lv_obj_get_height(object);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = commands[i];
        if (c.kind > 2 || c.width < 1 || c.width > 8 || c.reserved ||
            c.x1 < 0 || c.y1 < 0 || c.x2 < 0 || c.y2 < 0 ||
            c.x1 >= area_w || c.x2 >= area_w || c.y1 >= area_h || c.y2 >= area_h ||
            (c.kind && (c.x2 < c.x1 || c.y2 < c.y1))) {
            ESP_LOGE(TAG, "operation=draw stage=command result=invalid index=%lu area=%dx%d",
                     static_cast<unsigned long>(i), area_w, area_h);
            return XZ_APP_ERR_INVALID;
        }
    }
    // 仅重绘命令发生变化的区域，避免整屏刷新；相同命令直接跳过。
    uint32_t first = 0;
    while (first < count && first < drawing->commands.size() &&
           std::memcmp(&commands[first], &drawing->commands[first], sizeof(xz_app_draw_t)) == 0) {
        ++first;
    }
    if (first == count && first == drawing->commands.size()) {
        return XZ_APP_OK;
    }
    lv_area_t dirty{LV_COORD_MAX, LV_COORD_MAX, 0, 0};
    const auto include = [&dirty](const xz_app_draw_t& c) {
        dirty.x1 = std::min<int32_t>(dirty.x1, std::min(c.x1, c.x2) - c.width);
        dirty.y1 = std::min<int32_t>(dirty.y1, std::min(c.y1, c.y2) - c.width);
        dirty.x2 = std::max<int32_t>(dirty.x2, std::max(c.x1, c.x2) + c.width);
        dirty.y2 = std::max<int32_t>(dirty.y2, std::max(c.y1, c.y2) + c.width);
    };
    for (uint32_t i = first; i < count; ++i) {
        include(commands[i]);
    }
    for (uint32_t i = first; i < drawing->commands.size(); ++i) {
        include(drawing->commands[i]);
    }
    if (count) {
        drawing->commands.assign(commands, commands + count);
    } else {
        drawing->commands.clear();
    }
    lv_area_t origin;
    lv_obj_get_coords(object, &origin);
    dirty.x1 += origin.x1;
    dirty.x2 += origin.x1;
    dirty.y1 += origin.y1;
    dirty.y2 += origin.y1;
    lv_obj_invalidate_area(object, &dirty);
    return XZ_APP_OK;
}

static int SetAcceleration(void* host, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    state->acceleration_requested = enabled != 0;
    UpdateForeground(*state);
    return XZ_APP_OK;
}

static int GetAcceleration(void* host, xz_app_acceleration_t* result) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!result) {
        return XZ_APP_ERR_INVALID;
    }
    if (!state->acceleration_running) {
        return XZ_APP_ERR_NOT_READY;
    }
    int x = 0, y = 0, z = 0;
    if (!Accelerometer_GetAppGravity(&x, &y, &z)) {
        return XZ_APP_ERR_NOT_READY;
    }
    *result = {x, y, z};
    return XZ_APP_OK;
}

static uint64_t ForegroundMs(void* host) {
    auto* state = Active(host);
    if (state) {
        UpdateForeground(*state);
    }
    return state ? state->foreground_elapsed : 0;
}

static int SetOnBack(void* host, xz_app_back_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    state->on_back = callback;
    state->back_context = context;
    return XZ_APP_OK;
}

static int RequestScreenChange(void* host) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (AppUiSuspended() || lv_obj_get_screen(state->root) != lv_screen_active()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!state->transition_pending) {
        UpdateForeground(*state);
        state->transition_sequence = EpdGetStats().submitted;
        state->transition_pending = true;
    }
    UpdateForeground(*state);
    EpdRequestScreenChange();
    lv_obj_invalidate(state->root);
    return XZ_APP_OK;
}

static int Vibrate(void* host, uint32_t duration_ms) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!duration_ms || duration_ms > 15000) {
        return XZ_APP_ERR_INVALID;
    }
    if (AppUiSuspended() || state->transition_pending || lv_obj_get_screen(state->root) != lv_screen_active()) {
        return XZ_APP_ERR_NOT_READY;
    }
    const uint64_t now = MonotonicMs();
    // 系统点击反馈与应用震动共用马达，任一脉冲未结束都不叠加。
    if (Vibration_PulseActive() || (state->vibration_hold && now < state->vibration_until)) {
        return XZ_APP_ERR_BUSY;
    }
    state->vibration_id = HapticPulseForIfEnabled(static_cast<int>(duration_ms));
    if (!state->vibration_id) {
        return XZ_APP_OK;
    }
    state->vibration_until = now + duration_ms;
    if (!state->vibration_hold) {
        state->vibration_hold.emplace(PowerNeed::StandbyInhibit);
    }
    return XZ_APP_OK;
}

static int AudioGetCaps(void* host, xz_app_audio_caps_t* result) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    return AppAudioGetCaps(result);
}

static int AudioPlay(void* host, const xz_app_audio_config_t* config) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!state->audio) {
        return XZ_APP_ERR_MEMORY;
    }
    if (AppRecordBusy(state->recorder)) {
        return XZ_APP_ERR_BUSY;
    }
    return AppAudioPlay(state->audio, config);
}

static int AudioPause(void* host, int paused) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    return AppAudioPause(state->audio, paused != 0);
}

static int AudioStop(void* host) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    return AppAudioStop(state->audio);
}

static int AudioSetVolume(void* host, uint32_t volume) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    return AppAudioSetVolume(volume);
}

static int AudioGetStatus(void* host, xz_app_audio_status_t* result) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    return AppAudioGetStatus(state->audio, result);
}

static bool PrepareRecorder(AppRuntimeServicesState& state) {
    if (!state.recorder) {
        state.recorder = AppRecordCreate(state.storage_key);
    }
    if (!state.recorder) {
        return false;
    }
    UpdateForeground(state);
    return true;
}

static int RecordCaps(void* host, xz_app_audio_caps_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordCaps(result) : XZ_APP_ERR_NOT_READY;
}

static int RecordStart(void* host) {
    auto* state = Active(host);
    if (!state || state->closing || !state->foreground_timer) {
        return XZ_APP_ERR_NOT_READY;
    }
    xz_app_audio_caps_t caps{};
    caps.struct_size = sizeof(caps);
    const int supported = AppRecordCaps(&caps);
    if (supported != XZ_APP_OK) {
        return supported;
    }
    if (AppAudioBusy(state->audio)) {
        return XZ_APP_ERR_BUSY;
    }
    if (!PrepareRecorder(*state)) {
        return XZ_APP_ERR_MEMORY;
    }
    return AppRecordStart(state->recorder);
}

static int RecordPause(void* host, int paused) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordPause(state->recorder, paused != 0) : XZ_APP_ERR_NOT_READY;
}

static int RecordStop(void* host) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordStop(state->recorder) : XZ_APP_ERR_NOT_READY;
}

static int RecordStatus(void* host, xz_app_record_status_t* result) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    if (!PrepareRecorder(*state)) {
        return XZ_APP_ERR_MEMORY;
    }
    return AppRecordStatus(state->recorder, result);
}

static int FullRefresh(void* host) {
    auto* state = Active(host);
    auto* display = LVAdapterDisplay::Instance();
    if (!state || state->closing || !display || AppUiSuspended() ||
        lv_obj_get_screen(state->root) != lv_screen_active()) {
        return XZ_APP_ERR_NOT_READY;
    }
    UpdateForeground(*state);
    if (!state->full_refresh_pending) {
        state->full_refresh_pending = true;
        state->full_refresh_sequence = EpdGetStats().submitted;
        display->RequestFullOnNextCommit();
        lv_obj_invalidate(lv_screen_active());
    }
    return XZ_APP_OK;
}

static void* Image(void* host, void* parent, int x, int y, int width, int height) {
    auto* state = Active(host);
    return state && !state->closing ? AppImagesWidget(state->images, parent, x, y, width, height) : nullptr;
}

static int ImageLoad(void* host, void* widget, uint32_t offset, uint32_t length) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (state->camera_widget == widget) {
        AppCameraPreviewEnable(state->camera, false, 0, 0);
        state->camera_widget = nullptr;
    }
    return AppImagesLoad(state->images, widget, offset, length);
}

static int ImageEffects(void* host, void* widget, const xz_app_image_effects_t* effects) {
    auto* state = Active(host);
    return state && !state->closing ? AppImagesEffects(state->images, widget, effects) : XZ_APP_ERR_NOT_READY;
}

static int ImageStatus(void* host, void* widget, xz_app_image_status_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppImagesStatus(state->images, widget, result) : XZ_APP_ERR_NOT_READY;
}

static int ImageGetEffects(void* host, void* widget, xz_app_image_effects_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppImagesGetEffects(state->images, widget, result) : XZ_APP_ERR_NOT_READY;
}

static int ImageReadPixels(void* host, void* widget, uint32_t processed, uint32_t offset,
                            void* buffer, uint32_t length) {
    auto* state = Active(host);
    return state && !state->closing ? AppImagesReadPixels(state->images, widget, processed, offset, buffer, length) :
        XZ_APP_ERR_NOT_READY;
}

static int ImageSetPixels(void* host, void* widget, uint32_t width, uint32_t height,
                           const void* pixels, uint32_t length) {
    auto* state = Active(host);
    if (!state || state->closing) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (state->camera_widget == widget) {
        AppCameraPreviewEnable(state->camera, false, 0, 0);
        state->camera_widget = nullptr;
    }
    return AppImagesSetPixels(state->images, widget, width, height, pixels, length);
}

static int CameraPreview(void* host, void* widget, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing || AppUiSuspended() || lv_obj_get_screen(state->root) != lv_screen_active()) {
        return XZ_APP_ERR_NOT_READY;
    }
    int width = 0, height = 0;
    if (enabled && !AppImagesSize(state->images, widget, width, height)) {
        return XZ_APP_ERR_INVALID;
    }
    try {
        if (enabled && !state->camera) {
            state->camera = AppCameraPreviewCreate();
        }
        if (!state->camera) {
            return XZ_APP_OK;
        }
        if (enabled) {
            AppImagesPresent(state->images, widget, {});
        }
        const int result = AppCameraPreviewEnable(state->camera, enabled, width, height);
        if (result == XZ_APP_OK) {
            state->camera_widget = enabled ? widget : nullptr;
            UpdateForeground(*state);
        }
        return result;
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=camera_preview stage=request result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
}

static int CameraStatus(void* host, xz_app_camera_status_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppCameraPreviewStatus(state->camera, result) : XZ_APP_ERR_NOT_READY;
}

static void ForegroundTick(lv_timer_t* handle) {
    auto* state = static_cast<AppRuntimeServicesState*>(lv_timer_get_user_data(handle));
    if (!state->open) {
        return;
    }
    UpdateForeground(*state);
}

void AppRuntimeServicesBind(AppRuntimeServicesState& state, xz_app_api_t& api, lv_obj_t* root,
                            const AppStoreItem& item) {
    state.root = root;
    state.storage_key = AppStorageKey(item);
    state.audio = AppAudioCreate(item);
    state.network = AppNetworkCreate();
    state.open = true;
    try {
        state.images = AppImagesCreate(root, item, api.asset_size);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=images stage=create result=no_memory");
    }
    api.capabilities = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER | XZ_APP_CAP_WIDGETS | XZ_APP_CAP_EXIT | XZ_APP_CAP_NOTICE |
                       XZ_APP_CAP_KEEP_AWAKE | XZ_APP_CAP_DRAWING | XZ_APP_CAP_ACCELERATION | XZ_APP_CAP_BACK |
                       XZ_APP_CAP_SCREEN_CHANGE | XZ_APP_CAP_VIBRATION | XZ_APP_CAP_AUDIO;
    api.capabilities |= XZ_APP_CAP_FULL_REFRESH;
    if (!state.storage_key.empty()) {
        api.capabilities |= XZ_APP_CAP_STORAGE;
    }
    api.host_context = &state;
    api.get_local_time = LocalTime;
    api.monotonic_ms = MonotonicMs;
    api.timer_start = StartTimer;
    api.timer_cancel = CancelTimer;
    api.storage_read = ReadData;
    api.storage_write = WriteData;
    api.storage_remove = RemoveData;
    api.container = Container;
    api.input = Input;
    api.get_text = GetText;
    api.keyboard = Keyboard;
    api.keyboard_close = CloseKeyboard;
    api.progress = Progress;
    api.set_progress = SetProgress;
    api.set_on_exit = OnExit;
    api.notify = Notify;
    api.set_keep_awake = KeepAwake;
    api.clear_children = ClearChildren;
    api.drawing = Drawing;
    api.draw = Draw;
    api.set_acceleration = SetAcceleration;
    api.get_acceleration = GetAcceleration;
    api.foreground_ms = ForegroundMs;
    api.set_on_back = SetOnBack;
    api.request_screen_change = RequestScreenChange;
    api.vibrate = Vibrate;
    api.audio_get_caps = AudioGetCaps;
    api.audio_play = AudioPlay;
    api.audio_pause = AudioPause;
    api.audio_stop = AudioStop;
    api.audio_set_volume = AudioSetVolume;
    api.audio_get_status = AudioGetStatus;
    api.record_get_caps = RecordCaps;
    api.record_start = RecordStart;
    api.record_pause = RecordPause;
    api.record_stop = RecordStop;
    api.record_get_status = RecordStatus;
    api.request_full_refresh = FullRefresh;
    api.image = Image;
    api.image_load_asset = ImageLoad;
    api.image_set_effects = ImageEffects;
    api.image_get_status = ImageStatus;
    api.image_get_effects = ImageGetEffects;
    api.image_read_pixels = ImageReadPixels;
    api.image_set_pixels = ImageSetPixels;
    api.camera_preview = CameraPreview;
    api.camera_get_status = CameraStatus;
    api.network_open = [](void* host, const xz_app_net_config_t* config, uint32_t* id) {
        auto* service = ActiveNetwork(host);
        return service ? AppNetworkOpen(service->network, config, id) : XZ_APP_ERR_NOT_READY;
    };
    api.network_send = [](void* host, uint32_t id, const void* data, uint32_t length, uint32_t flags) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkSend(service->network, id, data, length, flags) : XZ_APP_ERR_NOT_READY;
    };
    api.network_get_status = [](void* host, uint32_t id, xz_app_net_status_t* result) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkStatus(service->network, id, result) : XZ_APP_ERR_NOT_READY;
    };
    api.network_read = [](void* host, uint32_t id, void* buffer, uint32_t capacity, uint32_t* flags) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkRead(service->network, id, buffer, capacity, flags) : XZ_APP_ERR_NOT_READY;
    };
    api.network_get_header = [](void* host, uint32_t id, const char* name, char* buffer, uint32_t capacity) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkHeader(service->network, id, name, buffer, capacity) : XZ_APP_ERR_NOT_READY;
    };
    api.network_close = [](void* host, uint32_t id) {
        auto* service = Active(host);
        return service ? AppNetworkClose(service->network, id) : XZ_APP_ERR_NOT_READY;
    };
    api.network_set_idle_timer = [](void* host, int enabled) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkSetIdleTimer(service->network, enabled) : XZ_APP_ERR_NOT_READY;
    };
    api.network_prepare = [](void* host, uint32_t timeout_ms, uint32_t* id) {
        auto* service = ActiveNetwork(host);
        return service ? AppNetworkPrepare(service->network, timeout_ms, id) : XZ_APP_ERR_NOT_READY;
    };
    api.network_resolve = [](void* host, const char* hostname, uint32_t timeout_ms, uint32_t* id) {
        auto* service = ActiveNetwork(host);
        return service ? AppNetworkResolve(service->network, hostname, timeout_ms, id) : XZ_APP_ERR_NOT_READY;
    };
    api.network_ping = [](void* host, const xz_app_ping_config_t* config, uint32_t* id) {
        auto* service = ActiveNetwork(host);
        return service ? AppNetworkPing(service->network, config, id) : XZ_APP_ERR_NOT_READY;
    };
    api.network_get_diagnostics = [](void* host, uint32_t id, xz_app_net_diagnostics_t* result) {
        auto* service = Active(host);
        return service && !service->closing ? AppNetworkDiagnostics(service->network, id, result) : XZ_APP_ERR_NOT_READY;
    };
    state.foreground_last = MonotonicMs() - StandbyPresentation::TotalDurationMs();
    state.foreground_timer = lv_timer_create(ForegroundTick, 100, &state);
    if (state.network && state.foreground_timer) {
        api.capabilities |= XZ_APP_CAP_NETWORK | XZ_APP_CAP_DNS | XZ_APP_CAP_PING;
    }
    if (state.foreground_timer && !state.storage_key.empty()) {
        api.capabilities |= XZ_APP_CAP_RECORDING;
    }
    if (state.foreground_timer && state.images) {
        api.capabilities |= XZ_APP_CAP_IMAGE | XZ_APP_CAP_CAMERA | XZ_APP_CAP_IMAGE_TONE | XZ_APP_CAP_IMAGE_PIXELS;
    }
    if (!state.foreground_timer) {
        ESP_LOGE(TAG, "operation=foreground_timer stage=allocate result=failed");
    }
}

void AppRuntimeServicesClose(AppRuntimeServicesState& state) {
    if (!state.open || state.closing) {
        return;
    }
    state.closing = true;
    AppNetworkStop(state.network, true);
    state.network.reset();
    AppCameraPreviewClose(state.camera);
    AppImagesClose(state.images);
    state.camera_widget = nullptr;
    state.camera.reset();
    state.images.reset();
    state.awake.reset();
    ReleaseAppVibration(state);
    if (state.acceleration_running) {
        state.acceleration_running = false;
        Accelerometer_SetAppTracking(false);
    }
    AppRecordClose(state.recorder);
    state.recorder.reset();
    AppAudioSetForeground(state.audio, false);
    AppAudioStop(state.audio);
    state.audio.reset();
    for (auto& timer : state.timers) {
        if (timer.handle) {
            lv_timer_delete(timer.handle);
            timer = {};
        }
    }
    if (state.foreground_timer) {
        lv_timer_delete(state.foreground_timer);
        state.foreground_timer = nullptr;
    }
    if (state.keyboard) {
        lv_obj_delete(state.keyboard);
    }
    if (state.on_exit) {
        state.on_exit(state.exit_context);
    }
    state.on_exit = nullptr;
    state.on_back = nullptr;
    state.open = false;
    ESP_LOGD(TAG, "operation=close result=success");
}

bool AppRuntimeServicesHandleBack(AppRuntimeServicesState& state) {
    if (!state.open || state.closing || !state.on_back || AppUiSuspended() ||
        lv_obj_get_screen(state.root) != lv_screen_active()) {
        return false;
    }
    return state.on_back(state.back_context) != 0;
}
