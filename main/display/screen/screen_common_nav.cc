#include "screen_common.h"
#include "screen_common_nav.h"
#include "screen_common_priv.h"

#include "application.h"
#include "home_screen/home_screen.h"
#include "lv_adapter_epdiy.h"
#include "vk_key_handler.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

/** S31 走 LvAdapterEpdiy，不用 esp_lv_adapter */
static bool ScreenAdapterReady() {
    auto* d = LvAdapterEpdiy::Instance();
    return d != nullptr && d->IsReady();
}

static bool ScreenAdapterLock(int timeout_ms) {
    auto* d = LvAdapterEpdiy::Instance();
    return d != nullptr && d->LockUi(timeout_ms);
}

static void ScreenAdapterUnlock() {
    if (auto* d = LvAdapterEpdiy::Instance()) {
        d->UnlockUi();
    }
}

void ScreenSetIsHome(bool is_home) {
    ScreenCommon_on_home = is_home;
}

bool ScreenIsHome() {
    return ScreenCommon_on_home;
}

void ScreenLoadReplace(lv_obj_t* new_scr) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_screen_load(new_scr);
    if (old_scr != nullptr && old_scr != new_scr) {
        lv_obj_delete_async(old_scr);
    }
    // 清掉跨页残留的 press/gesture，避免新页首击被当成旧拖拽吞掉
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset(indev, nullptr);
        }
    }
}

void ScreenNavigateTo(lv_obj_t* (*create)()) {
    if (create == nullptr) {
        return;
    }
    const char* cur = VkKey_ActiveScreen();
    if (cur != nullptr && std::strcmp(cur, kHomeScreen) != 0 &&
        std::strcmp(cur, "none") != 0) {
        if (ScreenFactory factory = VkKey_GetScreenFactory(cur)) {
            ScreenCommon_PushBackFactory(factory);
            ESP_LOGI(TAG, "navigate to: push back factory for %s (depth=%d)", cur, ScreenCommon_back_depth);
        }
    }
    ScreenCommon_on_home = false;
    ScreenLoadReplace(create());
}

void ScreenNavigateBack() {
    if (!ScreenAdapterReady()) {
        ESP_LOGW(TAG, "ScreenNavigateBack: adapter not ready");
        return;
    }
    const char* active = VkKey_ActiveScreen();
    if (active == nullptr || std::strcmp(active, kHomeScreen) == 0 ||
        std::strcmp(active, "none") == 0) {
        ESP_LOGI(TAG, "ScreenNavigateBack: skip stale back from screen=%s", active ? active : "null");
        return;
    }
    // 直接 lv_async_call，勿经主循环 Schedule；StartXiaozhiVoice 等可能长时间占住 MainEventLoop。
    if (lv_async_call(ScreenCommon_NavigateBackAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "ScreenNavigateBack: lv_async_call failed");
    }
}

void ScreenGoHome() {
    ScreenCommon_ClearBackStack();
    ScreenCommon_on_home = true;
    ScreenLoadReplace(HomeScreen::Create());
}

void ScreenRequestHome() {
    // 与 ScreenNavigateBack / 电源键同：勿经 MainEventLoop。
    // 长待机醒后 WiFi/蜂窝 EnsureNetworkReady、百问 StartXiaozhiVoice 等可占死主循环数秒～数十秒；
    // 若仍走 ScreenLvAsync，VkKey 有日志但 ScreenCommon_GoHomeAsync 迟迟不跑，表现为短按/长按都「没反应」。
    if (!ScreenLvAsyncUrgent(ScreenCommon_GoHomeAsync)) {
        ScreenLvAsync(ScreenCommon_GoHomeAsync);
    }
}

void ScreenRequestBack() {
    ScreenRequestHome();
}

bool ScreenLvAsync(void (*cb)(void*), void* user_data) {
    if (cb == nullptr) {
        return false;
    }
    if (!ScreenAdapterReady()) {
        ESP_LOGW(TAG, "ScreenLvAsync: adapter not ready");
        return false;
    }
    // 页码等状态由调用方已改完；此处只排队。主循环再抢锁，不堵 touch_feed。
    Application::GetInstance().Schedule([cb, user_data]() {
        if (!ScreenAdapterReady()) {
            return;
        }
        if (!ScreenAdapterLock(-1)) {
            ESP_LOGW(TAG, "ScreenLvAsync: adapter lock failed");
            return;
        }
        if (lv_async_call(cb, user_data) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "ScreenLvAsync: lv_async_call failed");
        }
        ScreenAdapterUnlock();
    });
    return true;
}

bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data) {
    if (cb == nullptr) {
        return false;
    }
    if (!ScreenAdapterReady()) {
        ESP_LOGW(TAG, "ScreenLvAsyncUrgent: adapter not ready");
        return false;
    }
    // 勿经 MainEventLoop：百问等网可占死主循环数十秒
    if (!ScreenAdapterLock(50)) {
        ESP_LOGW(TAG, "ScreenLvAsyncUrgent: adapter lock failed");
        return false;
    }
    const bool ok = lv_async_call(cb, user_data) == LV_RESULT_OK;
    if (!ok) {
        ESP_LOGW(TAG, "ScreenLvAsyncUrgent: lv_async_call failed");
    }
    ScreenAdapterUnlock();
    return ok;
}

void ScreenPaintCoalesceEnqueue(ScreenPaintCoalesce* c);

void ScreenPaintCoalesceAsync(void* user_data) {
    auto* c = static_cast<ScreenPaintCoalesce*>(user_data);
    if (c == nullptr || c->paint == nullptr) {
        return;
    }
    c->queued.store(false, std::memory_order_release);
    const uint32_t want = c->seq.load(std::memory_order_acquire);
    if (want == c->done.load(std::memory_order_acquire)) {
        return;
    }
    // 每次只画一帧；落后则立刻再排队（不再防抖），让 LVGL/墨水有机会 flush
    c->paint();
    c->done.store(want, std::memory_order_release);
    if (c->seq.load(std::memory_order_acquire) != c->done.load(std::memory_order_acquire)) {
        ScreenPaintCoalesceEnqueue(c);
    }
}

void ScreenPaintCoalesceEnqueue(ScreenPaintCoalesce* c) {
    if (c == nullptr || c->paint == nullptr) {
        return;
    }
    bool expected = false;
    if (!c->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!ScreenLvAsync(ScreenPaintCoalesceAsync, c)) {
        c->queued.store(false, std::memory_order_release);
    }
}

void ScreenPaintCoalesceDeferCb(void* arg) {
    ScreenPaintCoalesceEnqueue(static_cast<ScreenPaintCoalesce*>(arg));
}

bool ScreenPaintCoalesceEnsureTimer(ScreenPaintCoalesce* c) {
    if (c->defer_timer != nullptr) {
        return true;
    }
    esp_timer_handle_t t = nullptr;
    const esp_timer_create_args_t args = {
        .callback = &ScreenPaintCoalesceDeferCb,
        .arg = c,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "paint_coal",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &t) != ESP_OK || t == nullptr) {
        ESP_LOGW(TAG, "paint coalesce timer create failed");
        return false;
    }
    c->defer_timer = t;
    return true;
}

void ScreenPaintCoalesceArmDefer(ScreenPaintCoalesce* c, uint64_t delay_us) {
    if (!ScreenPaintCoalesceEnsureTimer(c)) {
        ScreenPaintCoalesceEnqueue(c);
        return;
    }
    auto* t = static_cast<esp_timer_handle_t>(c->defer_timer);
    esp_timer_stop(t);
    if (esp_timer_start_once(t, delay_us) != ESP_OK) {
        ScreenPaintCoalesceEnqueue(c);
    }
}

void ScreenPaintCoalesceRequest(ScreenPaintCoalesce* c) {
    if (c == nullptr || c->paint == nullptr) {
        return;
    }
    c->seq.fetch_add(1, std::memory_order_acq_rel);

    // 屏内连点：边沿还在队列 / 下一拍已按下 → 只记序号，让 LVGL 继续交 CLICKED
    // （固定短窗就开画会堵 indev，连点又变一页一刷）
    if (TouchUiHasPendingEdges() || TouchUiFingerIsDown()) {
        ScreenPaintCoalesceArmDefer(c, kPaintCoalescePendingFallbackUs);
        return;
    }

    // 触摸空闲（含刚交完最后一拍）：立刻排队画最终页；盖板键连点靠 seq 合并
    if (c->defer_timer != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(c->defer_timer));
    }
    ScreenPaintCoalesceEnqueue(c);
}

void ScreenPaintCoalesceRequestDebounced(ScreenPaintCoalesce* c, uint64_t delay_us) {
    if (c == nullptr || c->paint == nullptr) {
        return;
    }
    c->seq.fetch_add(1, std::memory_order_acq_rel);
    uint64_t wait = delay_us;
    if (wait == 0) {
        wait = kPaintCoalescePendingFallbackUs;
    }
    // 边沿未交完时至少撑过兜底窗，避免中途开画堵 indev
    if ((TouchUiHasPendingEdges() || TouchUiFingerIsDown()) &&
        wait < kPaintCoalescePendingFallbackUs) {
        wait = kPaintCoalescePendingFallbackUs;
    }
    ScreenPaintCoalesceArmDefer(c, wait);
}

void ScreenPaintCoalesceReset(ScreenPaintCoalesce* c) {
    if (c == nullptr) {
        return;
    }
    if (c->defer_timer != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(c->defer_timer));
    }
    c->queued.store(false, std::memory_order_release);
    const uint32_t seq = c->seq.load(std::memory_order_acquire);
    c->done.store(seq, std::memory_order_release);
}
