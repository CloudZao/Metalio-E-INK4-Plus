#include <new>
#include <esp_log.h>
#include "sdkconfig.h"
#include "sysmgmt/system_control.h"
#include "dual_network_board.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "infra/storage/settings.h"
#include "nt26_board.h"
#include "ui/common/drawing/display.h"
#include "wifi_board.h"

static const char* TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t cellular_tx_pin, gpio_num_t cellular_rx_pin, gpio_num_t cellular_dtr_pin,
                                   gpio_num_t cellular_ri_pin, int32_t default_net_type)
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
    #if CONFIG_PRODUCT_GLOBAL
    (void)default_net_type;
    return NetworkType::WIFI;
    #else
    Settings settings("network");
    // default_net_type: 1=4G, 0=WiFi；NVS 无记录时用该默认值
    const int fallback = (default_net_type == 0) ? 0 : 1;
    int network_type = settings.GetInt("type", fallback);
    return network_type == 1 ? NetworkType::CELLULAR : NetworkType::WIFI;
    #endif
}

bool DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) try {
    Settings settings("network", true);
    int network_type = (type == NetworkType::CELLULAR) ? 1 : 0;
    return settings.SetInt("type", network_type) && settings.Commit();
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=save_network_type result=no_memory");
    return false;
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::CELLULAR) {
        ESP_LOGI(TAG, "Initialize cellular board");

        current_board_ =
            std::make_unique<Nt26Board>(cellular_tx_pin_, cellular_rx_pin_, cellular_dtr_pin_, cellular_ri_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

bool DualNetworkBoard::SwitchToNetworkType(NetworkType type) {
    #if CONFIG_PRODUCT_GLOBAL
    if (type != NetworkType::WIFI) {
        return false;
    }
    #endif
    if (type == network_type_) {
        return true;
    }
    if (!SaveNetworkTypeToSettings(type)) {
        return false;
    }
    auto display = GetDisplay();
    if (display) {
        display->ShowNotification(type == NetworkType::CELLULAR ? LangString_SWITCH_TO_4G_NETWORK
                                                                : LangString_SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    SystemRequestReboot();
    return true;
}

std::string DualNetworkBoard::GetBoardType() {
    return BOARD_TYPE;
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

NetworkStatus DualNetworkBoard::GetNetworkStatus() {
    return current_board_->GetNetworkStatus();
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
