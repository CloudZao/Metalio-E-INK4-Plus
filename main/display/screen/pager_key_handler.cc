#include "pager_key_handler.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "battery_uv_lockout.h"
#include "boot_key_handler.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "power_policy.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {
using Action = BlePageTurner::Action;
using Phase = BlePageTurner::KeyPhase;
struct Event {
    Action action; // 已学习或默认的翻页、待机动作
    Phase phase; // 蓝牙侧识别的按键阶段
    uint32_t epoch; // 投递时的页面代次
    uint32_t generation; // 蓝牙输入代次，取消时作废积压事件
};
struct Mailbox {
    StaticQueue_t storage{};
    uint8_t bytes[16 * sizeof(Event)]{};
    QueueHandle_t queue = xQueueCreateStatic(16, sizeof(Event), bytes, &storage);
    std::atomic<bool> overflow{false};
    std::atomic<uint32_t> generation{0};
};
Mailbox& Events() { static Mailbox mailbox; return mailbox; }
}

void PagerKey_Receive(Action action, Phase phase) {
    auto& mailbox = Events();
    const auto generation = phase == Phase::kCancel ? mailbox.generation.fetch_add(1) + 1 : mailbox.generation.load();
    const Event event{action, phase, VkKey_InputEpoch(), generation};
    if (xQueueSend(mailbox.queue, &event, 0) != pdTRUE) mailbox.overflow.store(true);
}

void PagerKey_Drain() {
    BlePageTurner::Poll();
    auto& mailbox = Events();
    if (mailbox.overflow.exchange(false)) {
        xQueueReset(mailbox.queue);
        ESP_LOGW("PageTurner", "Input queue full; pending clicks discarded");
        return;
    }
    const bool blocked = BatteryUvLockout_IsActive() || OtaUpgradeScreen::IsActive() ||
        OtaConfirmDialog::IsActive() || StandbyScreen::IsActive();
    const auto state = BlePageTurner::GetSnapshot();
    Event event{};
    for (unsigned i = 0; i < 16 && xQueueReceive(mailbox.queue, &event, 0) == pdTRUE; ++i) {
        if (blocked || !state.enabled || state.learning || event.phase != Phase::kClick || event.generation != mailbox.generation.load() ||
            event.epoch != VkKey_InputEpoch()) continue;
        const char* action = event.action == Action::kStandby ? "standby" : event.action == Action::kPrevious ? "previous" : "next";
        ESP_LOGI("PageTurner", "Dispatch %s screen=%s", action, VkKey_ActiveScreen());
        PowerPolicy::GetInstance().NotifyUserActivity();
        if (event.action == Action::kStandby) {
            PowerKey_OnClick();
            xQueueReset(mailbox.queue);
            return;
        }
        VkKey_Dispatch(event.action == Action::kPrevious ? "vk_prev" : "vk_next");
    }
}
