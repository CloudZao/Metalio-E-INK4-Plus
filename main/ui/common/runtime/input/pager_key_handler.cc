#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "sysmgmt/update/update_presentation.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/input/pager_key_handler.h"
#include "ui/common/runtime/input/boot_key_handler.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/runtime/input/vk_key_handler.h"

using PagerKeyHandlerAction = BlePageTurnerAction;
using PagerKeyHandlerPhase = BlePageTurnerKeyPhase;

struct PagerKeyHandlerEvent {
    PagerKeyHandlerAction action;  // 已学习或默认的翻页、待机动作
    PagerKeyHandlerPhase phase;    // 蓝牙侧识别的按键阶段
    uint32_t epoch;                // 投递时的页面代次
    uint32_t generation;           // 蓝牙输入代次，取消时作废积压事件
};

struct PagerKeyHandlerMailbox {
    StaticQueue_t storage{};
    uint8_t bytes[16 * sizeof(PagerKeyHandlerEvent)]{};
    QueueHandle_t queue = xQueueCreateStatic(16, sizeof(PagerKeyHandlerEvent), bytes, &storage);
    std::atomic<bool> overflow{false};
    std::atomic<uint32_t> generation{0};
};

static PagerKeyHandlerMailbox& Events() {
    static PagerKeyHandlerMailbox mailbox;
    return mailbox;
}

void PagerKey_Receive(PagerKeyHandlerAction action, PagerKeyHandlerPhase phase) {
    auto& mailbox = Events();
    const auto generation =
        phase == PagerKeyHandlerPhase::kCancel ? mailbox.generation.fetch_add(1) + 1 : mailbox.generation.load();
    const PagerKeyHandlerEvent event{action, phase, VkKey_InputEpoch(), generation};
    if (xQueueSend(mailbox.queue, &event, 0) != pdTRUE) {
        mailbox.overflow.store(true);
    }
}

void PagerKey_Drain() {
    BlePageTurnerPoll();
    auto& mailbox = Events();
    if (mailbox.overflow.exchange(false)) {
        xQueueReset(mailbox.queue);
        ESP_LOGW("PageTurner", "Input queue full; pending clicks discarded");
        return;
    }
    const bool blocked = OtaUpgradeScreen::IsActive() || StandbyPresentation::IsActive();
    const auto state = BlePageTurnerGetSnapshot();
    PagerKeyHandlerEvent event{};
    for (unsigned i = 0; i < 16 && xQueueReceive(mailbox.queue, &event, 0) == pdTRUE; ++i) {
        if (blocked || !state.enabled || state.learning || event.phase != PagerKeyHandlerPhase::kClick ||
            event.generation != mailbox.generation.load() || event.epoch != VkKey_InputEpoch()) {
            continue;
        }
        const char* action = event.action == PagerKeyHandlerAction::kStandby    ? "standby"
                             : event.action == PagerKeyHandlerAction::kPrevious ? "previous"
                                                                                : "next";
        ESP_LOGI("PageTurner", "Dispatch %s screen=%s", action, VkKey_ActiveScreen());
        PowerPolicy::GetInstance().NotifyUserActivity();
        if (event.action == PagerKeyHandlerAction::kStandby) {
            PowerKey_OnClick();
            xQueueReset(mailbox.queue);
            return;
        }
        VkKey_Dispatch(event.action == PagerKeyHandlerAction::kPrevious ? "vk_prev" : "vk_next");
    }
}
