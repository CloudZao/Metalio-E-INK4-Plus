#include "screen_wifi.h"

#include "cloud_screen/ap/ap_transfer.h"
#include "dual_network_board.h"
#include "power_policy.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>

namespace {
std::atomic<ScreenWifiSwitchState> s_state{ScreenWifiSwitchState::kIdle};

void ToggleWifiTask(void* arg) {
    const bool enabled = arg != nullptr;
    esp_err_t result;
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        result = WifiStation::GetInstance().SetUserEnabled(enabled);
        if (result == ESP_OK && !enabled) ApTransferStop(false);
        PowerPolicy::GetInstance().NotifyUserActivity();
        PowerPolicy::GetInstance().RequestReevaluate();
    }
    s_state.store(result == ESP_OK ? ScreenWifiSwitchState::kIdle : ScreenWifiSwitchState::kFailed);
    vTaskDelete(nullptr);
}
} // namespace

ScreenWifiSwitchState ScreenWifiGetSwitchState() {
    return s_state.load();
}

void ScreenWifiToggle() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual && dual->GetNetworkType() != NetworkType::WIFI) return;
    if (s_state.exchange(ScreenWifiSwitchState::kSwitching) == ScreenWifiSwitchState::kSwitching) return;
    const bool enabled = !WifiStation::GetInstance().IsUserEnabled();
    if (xTaskCreate(ToggleWifiTask, "wifi_switch", 4096,
                    enabled ? reinterpret_cast<void*>(1) : nullptr, 5, nullptr) != pdPASS) {
        s_state.store(ScreenWifiSwitchState::kFailed);
        ESP_LOGW("ScreenWifi", "Wi-Fi switch task failed");
    }
}
