#include <algorithm>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>
#include "network_settings.h"
#include "wifi_setup.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "assets/lang_screen.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/board/nt26_board.h"
#include "boards/common/board/wifi_board.h"
#include "business/transfer/ap_transfer.h"
#include "infra/storage/settings.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "NetworkSettings";
static std::mutex s_mutex;
static std::weak_ptr<NetworkSettingsResult> s_request;
static std::atomic<int> s_sim_slot{0};
static std::atomic<NetworkWifiSwitchState> s_wifi_state{NetworkWifiSwitchState::kIdle};
static std::atomic<bool> s_modem_busy{false};

NetworkModemSession::NetworkModemSession() : acquired_(!s_modem_busy.exchange(true, std::memory_order_acq_rel)) {}

NetworkModemSession::~NetworkModemSession() {
    if (acquired_) {
        s_modem_busy.store(false, std::memory_order_release);
    }
}

static Nt26Board* SimBoard() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    return dynamic_cast<Nt26Board*>(dual ? &dual->GetCurrentBoard() : &board);
}

static bool SaveSimSlot(int slot) {
    Settings settings("network", true);
    return settings.SetInt("sim_slot", slot) && settings.Commit();
}

static int ParseSimSlot(const std::string& response) {
    size_t pos = response.find("\"SimSlot\"");
    if (pos == std::string::npos || (pos = response.find(',', pos)) == std::string::npos) {
        return -1;
    }
    pos = response.find_first_not_of(" \t", pos + 1);
    if (pos == std::string::npos || (response[pos] != '0' && response[pos] != '1')) {
        return -1;
    }
    if (pos + 1 < response.size() && response[pos + 1] >= '0' && response[pos + 1] <= '9') {
        return -1;
    }
    return response[pos] - '0';
}

static bool SendSimCommand(Nt26Board& board, const char* command, uint32_t timeout_ms, const char* stage) {
    std::string response;
    const esp_err_t error = board.SendAtCommand(command, response, timeout_ms, true);
    const bool ok = error == ESP_OK && response.find("OK") != std::string::npos;
    if (!ok) {
        ESP_LOGW(TAG, "operation=sim stage=%s result=failed error=%d", stage, int(error));
    }
    return ok;
}

struct SimRadioRecovery {
    Nt26Board& board;
    bool pending = true; // CFUN关闭尝试后，所有异常出口均需恢复射频
    ~SimRadioRecovery() {
        if (pending) {
            try {
                SendSimCommand(board, "AT+CFUN=1", 10000, "recover");
            } catch (const std::bad_alloc&) {
                ESP_LOGE(TAG, "operation=sim stage=recover result=no_memory");
            }
        }
    }
};

static void QuerySim(NetworkSettingsResult& result) {
    Settings settings("network");
    const int saved = settings.GetInt("sim_slot", 0);
    s_sim_slot.store(saved == 1 ? 1 : 0, std::memory_order_release);
    auto* board = SimBoard();
    if (!board) {
        result.ok = true;
        return;
    }
    result.progress.store(NetworkSettingsProgress::Query, std::memory_order_release);
    std::string response;
    const auto error = board->SendAtCommand("AT+ECSIMCFG?", response, 5000, true);
    const int slot = error == ESP_OK && response.find("OK") != std::string::npos ? ParseSimSlot(response) : -1;
    if (slot < 0) {
        ESP_LOGW(TAG, "operation=query_sim result=failed error=%d", int(error));
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_NET_CUR_DASH);
        return;
    }
    s_sim_slot.store(slot, std::memory_order_release);
    result.ok = slot == saved || SaveSimSlot(slot);
    if (!result.ok) {
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}

static void SwitchSim(NetworkSettingsResult& result) {
    auto* board = SimBoard();
    if (!board) {
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_NET_NO_4G);
        return;
    }
    SimRadioRecovery recovery{*board};
    const char* error = nullptr;
    result.progress.store(NetworkSettingsProgress::Offline, std::memory_order_release);
    if (!SendSimCommand(*board, "AT+CFUN=0", 8000, "offline")) {
        error = LangString_SETTINGS_NET_CFUN0_FAIL;
    } else {
        vTaskDelay(pdMS_TO_TICKS(500));
        result.progress.store(NetworkSettingsProgress::Select, std::memory_order_release);
        char command[64];
        std::snprintf(command, sizeof(command), "AT+ECSIMCFG=SimSlot,%d", result.target_slot);
        if (!SendSimCommand(*board, command, 5000, "select")) {
            error = LangString_SETTINGS_NET_ECSIMCFG_FAIL;
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
            result.progress.store(NetworkSettingsProgress::Online, std::memory_order_release);
            if (!SendSimCommand(*board, "AT+CFUN=1", 15000, "online")) {
                error = LangString_SETTINGS_NET_CFUN1_FAIL;
            } else {
                recovery.pending = false;
                s_sim_slot.store(result.target_slot, std::memory_order_release);
                result.ok = SaveSimSlot(result.target_slot);
                if (!result.ok) {
                    error = LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED;
                }
            }
        }
    }
    if (error) {
        std::snprintf(result.error, sizeof(result.error), "%s", error);
    }
    if (result.ok) {
        result.reboot_at_us.store(esp_timer_get_time() + 3200000, std::memory_order_release);
        result.progress.store(NetworkSettingsProgress::Restart, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(3200));
    }
}

static NetworkSettingsRequest SubmitNetworkSettings(NetworkSettingsOperation operation, int slot, NetworkType type) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto current = s_request.lock();
    if (current && !current->completed.load(std::memory_order_acquire)) {
        return {};
    }
    auto result = std::make_shared<NetworkSettingsResult>();
    result->operation = operation;
    result->target_type = type;
    result->target_slot = slot;
    if (!TaskExecutor::Schedule([result] {
        {
            PowerNeedHold hold(PowerNeed::StandbyInhibit);
            try {
                NetworkModemSession modem;
                if (!modem) {
                    std::snprintf(result->error, sizeof(result->error), "%s", LangString_SETTINGS_TEST_CELL_WAIT_RESOURCE);
                } else if (result->operation == NetworkSettingsOperation::QuerySim) {
                    QuerySim(*result);
                } else if (result->operation == NetworkSettingsOperation::SwitchSim) {
                    SwitchSim(*result);
                } else {
                    auto* board = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
                    result->ok = board && board->SwitchToNetworkType(result->target_type);
                    if (!result->ok) {
                        std::snprintf(result->error, sizeof(result->error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
                    }
                }
            } catch (const std::bad_alloc&) {
                std::snprintf(result->error, sizeof(result->error), "%s", LangString_COMMON_MEMORY_RETRY);
                ESP_LOGE(TAG, "operation=settings stage=execute result=no_memory");
            }
        }
        if (!result->ok) {
            ESP_LOGW(TAG, "operation=settings kind=%u result=failed", unsigned(result->operation));
        }
        result->completed.store(true, std::memory_order_release);
        if (result->operation == NetworkSettingsOperation::SwitchSim && result->ok) {
            SystemRequestReboot();
        }
    }, TaskDomain::Network)) {
        return {};
    }
    s_request = result;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=settings stage=submit result=no_memory");
    return {};
}

NetworkType NetworkSettingsCurrentType() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual ? dual->GetNetworkType() : SimBoard() ? NetworkType::CELLULAR : NetworkType::WIFI;
}

bool NetworkSettingsSupportsSim() {
    return SimBoard() != nullptr;
}

int NetworkSettingsSimSlot() {
    return s_sim_slot.load(std::memory_order_acquire);
}

NetworkSettingsRequest NetworkSettingsQuerySim() {
    return SubmitNetworkSettings(NetworkSettingsOperation::QuerySim, 0, NetworkType::WIFI);
}

NetworkSettingsRequest NetworkSettingsSwitchSim(int slot) {
    return slot == 0 || slot == 1 ? SubmitNetworkSettings(NetworkSettingsOperation::SwitchSim, slot, NetworkType::CELLULAR)
                                 : NetworkSettingsRequest{};
}

NetworkSettingsRequest NetworkSettingsSwitchType(NetworkType type) {
    return type == NetworkType::WIFI || type == NetworkType::CELLULAR
               ? SubmitNetworkSettings(NetworkSettingsOperation::SwitchNetwork, 0, type) : NetworkSettingsRequest{};
}

NetworkSettingsRequest NetworkSettingsCurrent() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto result = s_request.lock();
    return result && !result->completed.load(std::memory_order_acquire) ? result : NetworkSettingsRequest{};
}

std::shared_ptr<const NetworkSignalResult> NetworkSettingsSampleSignal() try {
    auto result = std::make_shared<NetworkSignalResult>();
    if (!TaskExecutor::Schedule([result] {
        {
            PowerNeedHold hold(PowerNeed::StandbyInhibit);
            try {
                auto* board = SimBoard();
                if (board) {
                    NetworkModemSession modem;
                    if (modem) {
                        const auto iccid = board->GetIccid();
                        std::snprintf(result->iccid, sizeof(result->iccid), "%s", iccid.c_str());
                        result->strength = board->GetSignalStrength();
                        result->kind = result->strength < 0 || result->strength == 99
                                           ? NetworkSignalKind::CellUnknown : NetworkSignalKind::CellSignal;
                    } else {
                        result->kind = NetworkSignalKind::CellUnavailable;
                    }
                } else if (dynamic_cast<WifiBoard*>(&Board::GetInstance()) ||
                           dynamic_cast<DualNetworkBoard*>(&Board::GetInstance())) {
                    auto& wifi = WifiStation::GetInstance();
                    if (!wifi.IsConnected()) {
                        result->kind = NetworkSignalKind::WifiDisconnected;
                    } else {
                        wifi_ap_record_t ap{};
                        if (wifi.TryGetApInfo(ap)) {
                            result->kind = NetworkSignalKind::WifiSignal;
                            result->strength = ap.rssi;
                        } else {
                            result->kind = NetworkSignalKind::WifiReadFailed;
                        }
                    }
                }
            } catch (const std::bad_alloc&) {
                ESP_LOGW(TAG, "operation=sample_signal result=no_memory");
            }
        }
        result->completed.store(true, std::memory_order_release);
    }, TaskDomain::Network)) {
        return {};
    }
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "operation=sample_signal stage=submit result=no_memory");
    return {};
}

NetworkWifiSwitchState NetworkWifiGetSwitchState() {
    return s_wifi_state.load(std::memory_order_acquire);
}

void NetworkWifiToggle() try {
    if (NetworkSettingsCurrentType() != NetworkType::WIFI ||
        s_wifi_state.exchange(NetworkWifiSwitchState::kSwitching) == NetworkWifiSwitchState::kSwitching) {
        return;
    }
    const bool enabled = !WifiStation::GetInstance().IsUserEnabled();
    if (TaskExecutor::Schedule([enabled] {
        esp_err_t error = ESP_FAIL;
        {
            PowerNeedHold hold(PowerNeed::OtaDownload);
            try {
                WifiSetupCloseCurrent(false);
                const auto deadline = esp_timer_get_time() + 30000000;
                while (!WifiSetupIsClosed() && esp_timer_get_time() < deadline) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                error = WifiSetupIsClosed() ? WifiStation::GetInstance().SetUserEnabled(enabled) : ESP_ERR_TIMEOUT;
                if (error == ESP_OK && !enabled) {
                    ApTransferStop(false);
                }
                PowerPolicy::GetInstance().NotifyUserActivity();
                PowerPolicy::GetInstance().RequestReevaluate();
            } catch (const std::bad_alloc&) {
                error = ESP_ERR_NO_MEM;
            }
        }
        s_wifi_state.store(error == ESP_OK ? NetworkWifiSwitchState::kIdle : NetworkWifiSwitchState::kFailed,
                           std::memory_order_release);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "operation=wifi_switch result=failed error=%d", int(error));
        }
    }, TaskDomain::Network)) {
        return;
    }
    s_wifi_state.store(NetworkWifiSwitchState::kFailed, std::memory_order_release);
} catch (const std::bad_alloc&) {
    s_wifi_state.store(NetworkWifiSwitchState::kFailed, std::memory_order_release);
    ESP_LOGE(TAG, "operation=wifi_switch stage=submit result=no_memory");
}
