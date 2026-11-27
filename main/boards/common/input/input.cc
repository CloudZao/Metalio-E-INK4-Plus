#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include "input.h"

static std::atomic<unsigned> s_sampling_pause_depth{0};  // 作用域共同持有的采样暂停引用

InputSamplingPause::InputSamplingPause() {
    s_sampling_pause_depth.fetch_add(1, std::memory_order_acq_rel);
}

InputSamplingPause::~InputSamplingPause() {
    s_sampling_pause_depth.fetch_sub(1, std::memory_order_acq_rel);
}

bool InputSamplingPaused() {
    return s_sampling_pause_depth.load(std::memory_order_acquire) != 0;
}

static std::atomic<bool> s_overflow{false};  // 界面需取消遗失的松手手势
static QueueHandle_t InputQueue() {
    static StaticQueue_t control;
    static uint8_t storage[32 * sizeof(InputEvent)];
    static QueueHandle_t queue = xQueueCreateStatic(32, sizeof(InputEvent), storage, &control);
    return queue;
}

bool InputPostEvent(InputKey key, InputAction action) {
    const InputEvent event{key, action};
    if (xQueueSend(InputQueue(), &event, 0) == pdTRUE) {
        return true;
    }
    if (!s_overflow.exchange(true)) {
        ESP_LOGW("Input", "operation=enqueue result=full; gestures will be cancelled");
    }
    return false;
}

bool InputTakeEvent(InputEvent* event) {
    return event && xQueueReceive(InputQueue(), event, 0) == pdTRUE;
}

bool InputTakeOverflow() {
    return s_overflow.exchange(false);
}
