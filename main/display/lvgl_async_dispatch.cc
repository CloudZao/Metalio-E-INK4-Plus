#include "screen/screen_common.h"

#include <esp_heap_caps.h>
#include <lvgl.h>
#include <mutex>

namespace {
struct PendingCall {
    lv_async_cb_t callback; // 取消后置空，调度记录仍由 UI 消费者回收。
    void* context; // 参数所有权遵循 LVGL 原接口，由调用方或回调管理。
    PendingCall* next; // 尚未开始执行的异步调用。
};

std::mutex s_pending_mutex;
PendingCall* s_pending;

void RemovePending(PendingCall* call) {
    auto** link = &s_pending;
    while (*link != call) link = &(*link)->next;
    *link = call->next;
}

void Dispatch(void* context) {
    auto* call = static_cast<PendingCall*>(context);
    lv_async_cb_t callback;
    void* user_data;
    {
        std::lock_guard<std::mutex> lock(s_pending_mutex);
        callback = call->callback;
        user_data = call->context;
        RemovePending(call);
    }
    heap_caps_free(call);
    if (callback != nullptr) callback(user_data);
}
}

/** @brief 将 LVGL 异步调用投递至统一 UI 队列，避免后台修改定时器链表。 */
extern "C" lv_result_t __wrap_lv_async_call(lv_async_cb_t callback, void* user_data) {
    if (callback == nullptr) return LV_RESULT_INVALID;
    auto* call = static_cast<PendingCall*>(heap_caps_malloc(sizeof(PendingCall),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (call == nullptr) return LV_RESULT_INVALID;
    bool queued;
    {
        std::lock_guard<std::mutex> lock(s_pending_mutex);
        *call = {callback, user_data, s_pending};
        s_pending = call;
        queued = ScreenLvAsync(Dispatch, call);
        if (!queued) RemovePending(call);
    }
    if (!queued) heap_caps_free(call);
    return queued ? LV_RESULT_OK : LV_RESULT_INVALID;
}

/** @brief 取消所有匹配且未开始执行的调用；参数仍由原调用方管理。 */
extern "C" lv_result_t __wrap_lv_async_call_cancel(lv_async_cb_t callback, void* user_data) {
    if (callback == nullptr) return LV_RESULT_INVALID;
    bool cancelled = false;
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    for (auto* call = s_pending; call != nullptr; call = call->next) {
        if (call->callback == callback && call->context == user_data) {
            call->callback = nullptr;
            cancelled = true;
        }
    }
    return cancelled ? LV_RESULT_OK : LV_RESULT_INVALID;
}
