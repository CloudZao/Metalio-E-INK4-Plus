#include "vk_page_repeat.h"

#include <atomic>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

#include "power_policy.h"

static constexpr const char* TAG = "VkPageRepeat";
static constexpr int kPagesPerSec = 10;
/** 轮询步长：对齐「满整秒」即可，不必 1Hz */
static constexpr uint64_t kPollIntervalUs = 100000;
static constexpr uint64_t kStepIntervalUs = 1000000;

static std::atomic<int> s_dir{0}; // -1 / +1
static std::atomic<VkPageRepeatStepFn> s_step{nullptr};
static std::atomic<int64_t> s_press_anchor_us{0}; // 手指按下时刻（TryStart 时刻回推门槛）
static std::atomic<int> s_applied_steps{0};      // 已应用的「整秒」档位数
static esp_timer_handle_t s_timer = nullptr;

static void StopLocked() {
    s_dir.store(0, std::memory_order_release);
    s_step.store(nullptr, std::memory_order_release);
    s_press_anchor_us.store(0, std::memory_order_release);
    s_applied_steps.store(0, std::memory_order_release);
    if (s_timer != nullptr) {
        esp_timer_stop(s_timer);
    }
}

static void PollOnce() {
    const int dir = s_dir.load(std::memory_order_acquire);
    VkPageRepeatStepFn step = s_step.load(std::memory_order_acquire);
    const int64_t anchor = s_press_anchor_us.load(std::memory_order_acquire);
    if (dir == 0 || step == nullptr || anchor <= 0) {
        return;
    }
    const int64_t elapsed_us = esp_timer_get_time() - anchor;
    if (elapsed_us < 0) {
        return;
    }
    const int target_steps = static_cast<int>(elapsed_us / static_cast<int64_t>(kStepIntervalUs));
    int applied = s_applied_steps.load(std::memory_order_acquire);
    while (applied < target_steps) {
        if (!step(dir * kPagesPerSec)) {
            StopLocked();
            return;
        }
        ++applied;
        s_applied_steps.store(applied, std::memory_order_release);
    }
}

static void TimerCb(void* /*arg*/) {
    PollOnce();
}

static bool EnsureTimer() {
    if (s_timer != nullptr) {
        return true;
    }
    const esp_timer_create_args_t args = {
        .callback = &TimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vk_page_rep",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGW(TAG, "timer create failed");
        s_timer = nullptr;
        return false;
    }
    return true;
}

extern "C" void VkPageRepeatStop(void) {
    StopLocked();
}

extern "C" bool VkPageRepeatIsActive(void) {
    return s_dir.load(std::memory_order_acquire) != 0;
}

extern "C" bool VkPageRepeatTryStart(const char* key, VkPageRepeatStepFn step) {
    if (key == nullptr || step == nullptr) {
        return false;
    }
    int dir = 0;
    if (std::strcmp(key, "vk_prev") == 0) {
        dir = -1;
    } else if (std::strcmp(key, "vk_next") == 0) {
        dir = 1;
    } else {
        return false;
    }
    if (!EnsureTimer()) {
        return true;
    }
    // TryStart 在长按门槛触发：回推门槛，使满 1s（自按下）才首次 ±10
    const int64_t now = esp_timer_get_time();
    const int64_t anchor = now - static_cast<int64_t>(kBootLongPressMs) * 1000;
    s_dir.store(dir, std::memory_order_release);
    s_step.store(step, std::memory_order_release);
    s_press_anchor_us.store(anchor, std::memory_order_release);
    s_applied_steps.store(0, std::memory_order_release);
    esp_timer_stop(s_timer);
    ESP_LOGI(TAG, "%s long-press -> repeat %s (1s/%d p, from press)", key,
             dir < 0 ? "prev" : "next", kPagesPerSec);
    if (esp_timer_start_periodic(s_timer, kPollIntervalUs) != ESP_OK) {
        ESP_LOGW(TAG, "timer start failed");
        StopLocked();
    }
    return true;
}

extern "C" bool VkPageRepeatOnPressUp(const char* key) {
    if (key == nullptr) {
        return false;
    }
    if (std::strcmp(key, "vk_prev") != 0 && std::strcmp(key, "vk_next") != 0) {
        return false;
    }
    if (!VkPageRepeatIsActive()) {
        return false;
    }
    ESP_LOGI(TAG, "%s press-up -> stop repeat", key);
    StopLocked();
    return true;
}
