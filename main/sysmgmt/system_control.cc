#include <atomic>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "system_control.h"
#include "system_state.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "SystemControl";
static void (*s_prepare_restart)() = nullptr;
static std::atomic<bool> s_restart_requested{false};
static std::atomic<bool> s_restart_queued{false};

void SystemControlInitialize(void (*prepare_restart)()) {
    s_prepare_restart = prepare_restart;
}

bool SystemRequestReboot() {
    if (s_prepare_restart == nullptr) {
        ESP_LOGE(TAG, "operation=reboot result=not_initialized");
        return false;
    }
    if (!s_restart_requested.exchange(true, std::memory_order_acq_rel)) {
        SystemState::GetInstance().BeginRestart();
        PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    }
    SystemControlPoll();
    return true;
}

void SystemControlPoll() {
    if (!s_restart_requested.load(std::memory_order_acquire) || s_restart_queued.exchange(true)) {
        return;
    }
    if (!TaskExecutor::Schedule(
            [] {
                struct RetryGuard {
                    ~RetryGuard() {
                        s_restart_queued.store(false, std::memory_order_release);
                    }
                } retry;
                s_prepare_restart();
                AudioRuntime::GetInstance().StopIfRunning(true);
                ESP_LOGI(TAG, "reboot prepared");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            },
            TaskDomain::Voice)) {
        s_restart_queued.store(false, std::memory_order_release);
    }
}
