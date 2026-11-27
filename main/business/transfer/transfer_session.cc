#include <atomic>
#include <mutex>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "transfer_session.h"
#include "infra/tasks/task_create.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "business/conversation/activation_polling.h"
#include "business/network/wifi_setup.h"
#include "sysmgmt/power/policy/power_policy.h"

static constexpr const char* TAG = "TransferSession";
static constexpr uint32_t kStackBytes = 8192;
static std::mutex s_mutex;
static std::atomic<uint32_t> s_generation{0};
static TransferSessionMode s_mode = TransferSessionMode::Closed;
static TaskHandle_t s_task = nullptr;
static bool s_stop_pending = false;
static bool s_power_held = false;
static bool s_activation_paused = false;
static TransferSessionSnapshot s_snapshot;

static void SetSessionHolds(TransferSessionMode mode) {
    const bool power = mode == TransferSessionMode::Cloud || mode == TransferSessionMode::Hotspot ||
                       mode == TransferSessionMode::Wifi;
    const bool activation = mode == TransferSessionMode::Hotspot || mode == TransferSessionMode::Wifi ||
                            mode == TransferSessionMode::Usb;
    if (power != s_power_held) {
        s_power_held = power;
        if (power) {
            PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
            PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
        } else {
            PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
            PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        }
    }
    if (activation != s_activation_paused) {
        s_activation_paused = activation;
        if (activation) {
            ActivationPolling::GetInstance().Pause();
        } else {
            ActivationPolling::GetInstance().Resume();
        }
    }
}

static void RunSession(void*) {
    for (;;) {
        uint32_t generation;
        TransferSessionMode mode;
        bool stop;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            generation = s_generation.load();
            mode = s_mode;
            stop = s_stop_pending;
            s_stop_pending = false;
        }
        ApTransferInfo info{};
        bool ok = true;
        try {
            WifiSetupCloseCurrent(mode != TransferSessionMode::Standby);
            const auto close_deadline = esp_timer_get_time() + 30000000;
            while (!WifiSetupIsClosed() && s_generation.load() == generation &&
                   esp_timer_get_time() < close_deadline) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (s_generation.load() != generation) {
                continue;
            }
            if (!WifiSetupIsClosed()) {
                ok = false;
                info.fail = LangString_CLOUD_NET_NOT_READY;
            }
            if (ok && stop) {
                ApTransferStop(mode != TransferSessionMode::Usb && mode != TransferSessionMode::Standby);
            }
            if (s_generation.load() != generation) {
                continue;
            }
            if (ok && mode == TransferSessionMode::Hotspot) {
                ok = ApTransferStart(&info);
            } else if (ok && !stop && mode != TransferSessionMode::Standby && mode != TransferSessionMode::Closed) {
                ApTransferStop(mode != TransferSessionMode::Usb);
            }
        } catch (const std::bad_alloc&) {
            ok = false;
            info.fail = LangString_COMMON_MEMORY_RETRY;
            ESP_LOGE(TAG, "operation=switch_mode stage=allocate result=no_memory");
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_generation.load() == generation) {
                s_snapshot = {generation, mode, true, ok, info};
                if (mode == TransferSessionMode::Closed || mode == TransferSessionMode::Standby) {
                    s_task = nullptr;
                    break;
                }
            }
        }
        // 会话期间保留控制线程，保证取消不依赖再次分配线程或排队到正在排空的传输通道。
        while (s_generation.load() == generation) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
    vTaskDelete(nullptr);
}

uint32_t TransferSessionRequest(TransferSessionMode mode) {
    if (mode < TransferSessionMode::Standby || mode > TransferSessionMode::Wifi) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_mode = mode;
    const auto generation = s_generation.fetch_add(1) + 1;
    s_snapshot = {generation, mode, false, false, {}};
    const bool closing = mode == TransferSessionMode::Closed || mode == TransferSessionMode::Standby;
    if (closing) {
        ApTransferRequestStop(mode == TransferSessionMode::Standby);
        s_stop_pending = s_task != nullptr || s_stop_pending;
    }
    if (mode != TransferSessionMode::Standby) {
        SetSessionHolds(mode);
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
        return generation;
    }
    if (closing && !s_stop_pending) {
        s_snapshot.ready = true;
        s_snapshot.ok = true;
        return generation;
    }
    if (FirmwareCreateTask(RunSession, "transfer_mode", kStackBytes, nullptr, 2, &s_task, tskNO_AFFINITY, 0) == pdPASS) {
        return generation;
    }
    s_task = nullptr;
    SetSessionHolds(TransferSessionMode::Closed);
    ESP_LOGW(TAG, "operation=switch_mode stage=start result=unavailable");
    return 0;
}

TransferSessionSnapshot TransferSessionRead() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot;
}
