#include "screen_frontlight.h"
#include "screen_common.h"
#include "screen_common_nav.h"
#include "screen_common_priv.h"

#include "home_screen/home_screen.h"
#include "book_screen/book_nav.h"
#include "display_orient.h"
#include "lv_adapter_epdiy.h"
#include "epd_refresh.h"
#include "vk_key_handler.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>

namespace {
struct UiMessage {
    void (*callback)(void*); // 仅由 UI 线程执行
    void* context; // 接收成功后交给回调释放
};

struct UiMailboxes {
    static constexpr size_t kNormalDepth = 80; // 普通结果和进度消息
    static constexpr size_t kUrgentDepth = 16; // 导航、待机等优先消息
    StaticQueue_t normal_storage{}, urgent_storage{}; // 静态队列控制块
    uint8_t normal_buffer[kNormalDepth * sizeof(UiMessage)]{}; // 有界普通消息缓冲
    uint8_t urgent_buffer[kUrgentDepth * sizeof(UiMessage)]{}; // 独立优先缓冲
    QueueHandle_t normal = xQueueCreateStatic(kNormalDepth, sizeof(UiMessage), normal_buffer, &normal_storage);
    QueueHandle_t urgent = xQueueCreateStatic(kUrgentDepth, sizeof(UiMessage), urgent_buffer, &urgent_storage);
};

UiMailboxes& Mailboxes() {
    static UiMailboxes mailboxes;
    return mailboxes;
}
}

/** S31 走 LvAdapterEpdiy，不用 esp_lv_adapter */
static bool ScreenAdapterReady() {
    auto* d = LvAdapterEpdiy::Instance();
    return d != nullptr && d->IsReady();
}

void ScreenSetIsHome(bool is_home) {
    ScreenCommon_on_home = is_home;
}

bool ScreenIsHome() {
    return ScreenCommon_on_home;
}

void ScreenLoadReplace(lv_obj_t* new_scr) {
    ScreenFrontlightHide();
    lv_obj_t* old_scr = lv_screen_active();
    EpdCancelPageTurn();
    EpdRequestScreenChange();
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
    if (ScreenFrontlightIsVisible()) {
        ScreenFrontlightHide();
        return;
    }
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
    // 调用方已持有 UI 锁，延后导航到下一次 LVGL 定时器处理。
    if (lv_async_call(ScreenCommon_NavigateBackAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "ScreenNavigateBack: lv_async_call failed");
    }
}

void ScreenGoHome() {
    // 回首页入口可能来自默认按键策略：必须在创建竖屏首页前退出阅读。
    ReleaseReaderForSystemHome();
    if (DisplayUiSetOrient(kDisplayUiPortrait)) {
        if (auto* disp = LvAdapterEpdiy::Instance()) {
            disp->RequestFullOnNextCommit();
        }
    }
    ScreenCommon_ClearBackStack();
    ScreenCommon_on_home = true;
    ScreenLoadReplace(HomeScreen::Create());
}

void ScreenRequestHome() {
    // 导航优先进入 UI 队列，避免普通结果和重绘消息延迟主页操作。
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
    const UiMessage message{cb, user_data};
    return xQueueSend(Mailboxes().normal, &message, 0) == pdTRUE;
}

bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data) {
    if (cb == nullptr) {
        return false;
    }
    if (!ScreenAdapterReady()) {
        ESP_LOGW(TAG, "ScreenLvAsyncUrgent: adapter not ready");
        return false;
    }
    const UiMessage message{cb, user_data};
    return xQueueSend(Mailboxes().urgent, &message, 0) == pdTRUE;
}

void ScreenLvAsyncDrain() {
    auto& queues = Mailboxes();
    const int64_t begin = esp_timer_get_time();
    UiMessage message{};
    for (unsigned count = 0; count < 8; ++count) {
        if (xQueueReceive(queues.urgent, &message, 0) != pdTRUE &&
            xQueueReceive(queues.normal, &message, 0) != pdTRUE) break;
        message.callback(message.context);
        // 单个回调可能绘制整页；本轮完成后交还输入采样与刷新。
        if (esp_timer_get_time() - begin >= 5000) break;
    }
}

void ScreenPaintCoalesceEnqueue(ScreenPaintCoalesce* c);
void ScreenPaintCoalesceArmDefer(ScreenPaintCoalesce* c, uint64_t delay_us);
bool ScreenPaintCoalesceEnsureTimer(ScreenPaintCoalesce* c);

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
    // Chapter parsing, pagination and widget allocation compete with LCD DMA
    // refill. Keep the request pending until the previous waveform finishes.
    if (c->defer_while_epd_busy && EpdGetStats().busy) {
        ScreenPaintCoalesceArmDefer(c, 20000);
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
    // 重绘直接进入 UI 队列，与网络和语音任务独立。
    if (ScreenLvAsync(ScreenPaintCoalesceAsync, c)) return;
    c->queued.store(false, std::memory_order_release);
    // 不阻塞触摸或定时器任务；队列满时保留序号并稍后重试。
    if (ScreenAdapterReady() && ScreenPaintCoalesceEnsureTimer(c)) {
        auto* timer = static_cast<esp_timer_handle_t>(c->defer_timer.load());
        esp_timer_stop(timer);
        if (esp_timer_start_once(timer, 20000) != ESP_OK) {
            ESP_LOGW(TAG, "paint coalesce retry start failed");
        }
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
    void* expected = nullptr;
    if (!c->defer_timer.compare_exchange_strong(expected, t)) esp_timer_delete(t);
    return true;
}

void ScreenPaintCoalesceArmDefer(ScreenPaintCoalesce* c, uint64_t delay_us) {
    if (!ScreenPaintCoalesceEnsureTimer(c)) {
        ScreenPaintCoalesceEnqueue(c);
        return;
    }
    auto* t = static_cast<esp_timer_handle_t>(c->defer_timer.load());
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
        esp_timer_stop(static_cast<esp_timer_handle_t>(c->defer_timer.load()));
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
        esp_timer_stop(static_cast<esp_timer_handle_t>(c->defer_timer.load()));
    }
    c->queued.store(false, std::memory_order_release);
    const uint32_t seq = c->seq.load(std::memory_order_acquire);
    c->done.store(seq, std::memory_order_release);
}
