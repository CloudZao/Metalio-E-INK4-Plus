#include "assets/lang_cellular.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "wifi_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "sdkconfig.h"
#include <esp_log.h>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t cellular_tx_pin,
    gpio_num_t cellular_rx_pin,
    gpio_num_t cellular_dtr_pin,
    gpio_num_t cellular_ri_pin,
    int32_t default_net_type)
: Board(),
cellular_tx_pin_(cellular_tx_pin),
cellular_rx_pin_(cellular_rx_pin),
cellular_dtr_pin_(cellular_dtr_pin),
cellular_ri_pin_(cellular_ri_pin) {
// 从Settings加载网络类型
network_type_ = LoadNetworkTypeFromSettings(default_net_type);

// 只初始化当前网络类型对应的板卡
InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    (void)default_net_type;
    return NetworkType::WIFI;
    #else
    Settings settings("network", true);
    // default_net_type: 1=4G, 0=WiFi；NVS 无记录时用该默认值
    const int fallback = (default_net_type == 0) ? 0 : 1;
    int network_type = settings.GetInt("type", fallback);
    return network_type == 1 ? NetworkType::CELLULAR : NetworkType::WIFI;
    #endif
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::CELLULAR) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::CELLULAR) {
        ESP_LOGI(TAG, "Initialize cellular board");

        current_board_ = std::make_unique<Nt26Board>(
            cellular_tx_pin_, cellular_rx_pin_, cellular_dtr_pin_, cellular_ri_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchToNetworkType(NetworkType type) {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    if (type != NetworkType::WIFI) {
        return;
    }
    #endif
    if (type == network_type_) {
        return;
    }
    auto display = GetDisplay();
    SaveNetworkTypeToSettings(type);
    if (type == NetworkType::CELLULAR) {
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    Application::GetInstance().Reboot();
}

std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    current_board_->StartNetwork();
}

bool DualNetworkBoard::EnsureNetworkReady(int timeout_ms) {
    return current_board_->EnsureNetworkReady(timeout_ms);
}

void DualNetworkBoard::RefreshNetworkWaitDeadline(int timeout_ms) {
    current_board_->RefreshNetworkWaitDeadline(timeout_ms);
}

void DualNetworkBoard::PauseNetworkIfNotReady() {
    current_board_->PauseNetworkIfNotReady();
}

bool DualNetworkBoard::IsNetworkReady() {
    return current_board_->IsNetworkReady();
}

bool DualNetworkBoard::IsWifiConfigMode() {
    return current_board_->IsWifiConfigMode();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
