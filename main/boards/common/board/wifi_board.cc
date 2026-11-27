#include <algorithm>
#include <memory>
#include <new>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_network.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_station.h>
#include <cJSON.h>
#include <font_awesome.h>
#include "wifi_board.h"
#include "boards/common/audio/audio_codec.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "boards/common/power/power_hw.h"
#include "infra/storage/settings.h"
#include "infra/network/wifi_radio.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "infra/platform/system_info.h"
#include "ui/common/drawing/display.h"

static const char* TAG = "WifiBoard";

static portMUX_TYPE s_ensure_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_ensure_deadline_us = 0;
static bool s_ensure_has_leader = false;

static constexpr int kEnsureSliceMs = 200;
static constexpr int kEnsureGiveUpPauseMs = 30000;                    // 整段等网失败后 Pause STA
static constexpr int64_t kEnsureWaitLogIntervalUs = 3 * 1000 * 1000;  // 未连上网时 enter/fail 日志限流
static int64_t s_last_ensure_wait_log_us = 0;

static void BumpEnsureDeadlineUs(int64_t deadline_us) {
    portENTER_CRITICAL(&s_ensure_mux);
    if (deadline_us > s_ensure_deadline_us) {
        s_ensure_deadline_us = deadline_us;
    }
    portEXIT_CRITICAL(&s_ensure_mux);
}

// 传输热点占用射频时不要改回 STA
static bool SoftApOwnsRadio() {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return false;
    }
    return (mode & WIFI_MODE_AP) != 0 && (mode & WIFI_MODE_STA) == 0;
}

// STA 关/未起时拉起；已连上则 no-op
static void EnsureWifiRadioUp(WifiStation& wifi) {
    if (WifiRadio::GetInstance().IsStationSetupActive()) {
        return;
    }
    if (!wifi.IsUserEnabled() || wifi.IsConnected() || SoftApOwnsRadio()) {
        return;
    }
    if (wifi.IsLpPaused()) {
        power_hw_main_rail_set(true);
        if (wifi.ResumeFromLp() != ESP_OK) {
            wifi.Start();
        }
        return;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        wifi.Start();
    }
}

// 落墨中推迟起射频；timeout_ms 为最长等待，仍忙则 false
static bool WaitPanelThenRadioOk(int timeout_ms) {
    if (!Display::IsPanelBusy()) {
        return true;
    }
    const int wait_ms = timeout_ms <= 0 ? 50 : (timeout_ms > 2000 ? 2000 : timeout_ms);
    return Display::WaitPanelIdle(wait_ms);
}

void WifiBoard::EnterWifiConfigMode() {
    // 网页 SoftAP 配网已移除：请到「设置 → 网络 → 配置 WIFI 网络」机上扫网配网。
    // 保留函数以免旧 force_ap / 板级 BOOT 路径崩掉；不再阻塞主启动。
    ESP_LOGW(TAG, "SoftAP web config removed; use Settings→Network WiFi setup");
    wifi_config_mode_ = false;
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(LangString_SETTINGS_NET_WIFI_CFG_HINT, 5000);
    }
}

std::string WifiBoard::GetBoardJson() try {
    const auto failed = []() {
        ESP_LOGE(TAG, "operation=board_snapshot stage=encode result=no_memory");
        return std::string{};
    };
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "type", BOARD_TYPE) ||
        !cJSON_AddStringToObject(root.get(), "name", BOARD_NAME)) {
        return failed();
    }
    auto& station = WifiStation::GetInstance();
    if (!wifi_config_mode_) {
        wifi_ap_record_t ap_info = {};
        const bool available = station.TryGetApInfo(ap_info);
        if (!cJSON_AddStringToObject(root.get(), "ssid", station.GetSsid().c_str()) ||
            !cJSON_AddNumberToObject(root.get(), "rssi", available ? ap_info.rssi : WifiStation::kRssiUnavailable) ||
            !cJSON_AddNumberToObject(root.get(), "channel",
                                     available ? ap_info.primary : WifiStation::kChannelUnavailable) ||
            !cJSON_AddStringToObject(root.get(), "ip", station.GetIpAddress().c_str())) {
            return failed();
        }
    }
    if (!cJSON_AddStringToObject(root.get(), "mac", SystemInfo::GetMacAddress().c_str())) {
        return failed();
    }
    std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!encoded) {
        return failed();
    }
    return std::string(encoded.get());
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=board_snapshot stage=allocate result=no_memory");
    return {};
}

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    if (PowerLifecycleIsStandby()) {
        return;
    }
    if (wifi_config_mode_) {
        // 旧 NVS force_ap：忽略 SoftAP，继续走正常 STA（或无凭证直接返回）
        ESP_LOGW(TAG, "force_ap ignored (no SoftAP); continue without web config");
        wifi_config_mode_ = false;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "No WiFi SSID; skip SoftAP. Configure in Settings→Network");
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([]() {
        auto display = Board::GetInstance().GetDisplay();
        display->UpdateStatusBar(true);
    });
    wifi_station.OnConnect([](const std::string& ssid) {
        (void)ssid;
        auto display = Board::GetInstance().GetDisplay();
        display->UpdateStatusBar(true);
    });
    wifi_station.OnConnected([](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = LangString_CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 3000);
        display->UpdateStatusBar(true);
    });
    if (!wifi_station.IsUserEnabled()) {
        return;
    }
    wifi_station.Start();

    // 永不保网仍仅等待 30 秒启动联网，超时后保留后台连接。
    const int grace_sec = PowerPolicy::GetInstance().GetNetGraceSec();
    const int wait_ms = (grace_sec == 0 ? 30 : grace_sec) * 1000;
    bool connected = false;
    for (int elapsed = 0; elapsed < wait_ms; elapsed += 50) {
        if (!wifi_station.IsUserEnabled()) {
            return;
        }
        if (PowerLifecycleIsStandby()) {
            PowerPolicy::GetInstance().RequestReevaluate();
            ESP_LOGI(TAG, "boot WiFi wait cancelled for standby");
            return;
        }
        if (wifi_station.WaitForConnected(std::min(50, wait_ms - elapsed))) {
            connected = true;
            break;
        }
    }
    if (!connected) {
        ESP_LOGW(TAG, "WiFi not connected within %d ms; leave network to power policy", wait_ms);
        // 统一由策略决定停网，避免与选项切换及业务占网并发执行 Pause。
        PowerPolicy::GetInstance().RequestReevaluate();
        return;
    }
}

bool WifiBoard::EnsureNetworkReady(int timeout_ms) {
    if (WifiRadio::GetInstance().IsStationSetupActive()) {
        return false;
    }
    auto& wifi = WifiStation::GetInstance();
    if (!wifi.IsUserEnabled()) {
        return false;
    }
    const char* task = pcTaskGetName(nullptr);
    const int core = xPortGetCoreID();
    const UBaseType_t stack_before = uxTaskGetStackHighWaterMark(nullptr);
    const int64_t t0 = esp_timer_get_time();
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    const bool already = wifi.IsConnected();
    // 百问等短切片每 200ms 进一次：未连上时 enter/fail 最多 3s 打一对，避免刷屏
    const bool log_wait = !already && (t0 - s_last_ensure_wait_log_us >= kEnsureWaitLogIntervalUs);
    if (already || log_wait) {
        ESP_LOGI(TAG, "EnsureNetworkReady: enter task=%s core=%d timeout_ms=%d stack_hwm=%u connected=%d",
                 task != nullptr ? task : "?", core, timeout_ms, static_cast<unsigned>(stack_before), already ? 1 : 0);
    }
    if (already) {
        return true;
    }
    // SoftAP 配网无 STA 互联网，空等只会拖死主循环/业务线程
    if (wifi_config_mode_) {
        ESP_LOGW(TAG, "EnsureNetworkReady: skip (wifi config mode) task=%s", task != nullptr ? task : "?");
        return false;
    }
    if (SoftApOwnsRadio()) {
        ESP_LOGW(TAG, "EnsureNetworkReady: skip (softAP) task=%s", task != nullptr ? task : "?");
        return false;
    }

    // 墨水落墨中勿起/扫射频：流式行队列缓冲短，WiFi bring-up 易 underrun
    if (!WaitPanelThenRadioOk(timeout_ms)) {
        if (log_wait) {
            ESP_LOGW(TAG, "EnsureNetworkReady: defer radio (panel busy) task=%s", task != nullptr ? task : "?");
        }
        return false;
    }

    EnsureWifiRadioUp(wifi);
    if (wifi.IsConnected()) {
        return true;
    }

    const int64_t personal_dl = t0 + static_cast<int64_t>(timeout_ms) * 1000LL;
    BumpEnsureDeadlineUs(personal_dl);

    // 单飞：同一时刻仅一个 leader 切片 WaitForConnected；后来者延长 shared deadline 并跟随
    // 个人 deadline 到点即返回（百问 200ms 切片不被 30s 传输等网拖死）
    while (wifi.IsUserEnabled() && !wifi.IsConnected()) {
        if (SoftApOwnsRadio()) {
            return false;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= personal_dl) {
            break;
        }

        bool lead = false;
        int64_t shared_dl = 0;
        portENTER_CRITICAL(&s_ensure_mux);
        if (!s_ensure_has_leader) {
            s_ensure_has_leader = true;
            lead = true;
        }
        shared_dl = s_ensure_deadline_us;
        portEXIT_CRITICAL(&s_ensure_mux);

        if (lead) {
            while (wifi.IsUserEnabled() && !wifi.IsConnected() && !SoftApOwnsRadio()) {
                const int64_t n2 = esp_timer_get_time();
                portENTER_CRITICAL(&s_ensure_mux);
                shared_dl = s_ensure_deadline_us;
                portEXIT_CRITICAL(&s_ensure_mux);
                if (n2 >= personal_dl || n2 >= shared_dl) {
                    break;
                }
                int64_t rem_us = std::min(personal_dl, shared_dl) - n2;
                int ms = static_cast<int>(rem_us / 1000);
                if (ms > kEnsureSliceMs) {
                    ms = kEnsureSliceMs;
                }
                if (ms < 1) {
                    break;
                }
                wifi.WaitForConnected(ms);
            }
            portENTER_CRITICAL(&s_ensure_mux);
            s_ensure_has_leader = false;
            portEXIT_CRITICAL(&s_ensure_mux);
            continue;
        }

        int ms = static_cast<int>((personal_dl - now) / 1000);
        if (ms > kEnsureSliceMs) {
            ms = kEnsureSliceMs;
        }
        if (ms < 1) {
            break;
        }
        wifi.WaitForConnected(ms);
    }

    const UBaseType_t stack_after = uxTaskGetStackHighWaterMark(nullptr);
    if (wifi.IsConnected()) {
        ESP_LOGI(TAG,
                 "EnsureNetworkReady: WiFi ready ip=%s task=%s core=%d elapsed_ms=%d "
                 "stack_hwm=%u->%u",
                 wifi.GetIpAddress().c_str(), task != nullptr ? task : "?", core,
                 static_cast<int>((esp_timer_get_time() - t0) / 1000), static_cast<unsigned>(stack_before),
                 static_cast<unsigned>(stack_after));
        return true;
    }
    if (log_wait) {
        ESP_LOGW(TAG,
                 "EnsureNetworkReady: WiFi not connected within %d ms task=%s core=%d "
                 "elapsed_ms=%d stack_hwm=%u->%u",
                 timeout_ms, task != nullptr ? task : "?", core, static_cast<int>((esp_timer_get_time() - t0) / 1000),
                 static_cast<unsigned>(stack_before), static_cast<unsigned>(stack_after));
        s_last_ensure_wait_log_us = esp_timer_get_time();
    }
    // 整段等网失败：停扫省电；短切片（百问/翻译）由预算循环结束时 PauseNetworkIfNotReady
    if (timeout_ms >= kEnsureGiveUpPauseMs && !SoftApOwnsRadio()) {
        ESP_LOGI(TAG, "EnsureNetworkReady give-up (>=%d ms) -> Pause STA", kEnsureGiveUpPauseMs);
        wifi.PauseForLp();
    }
    return false;
}

const char* WifiBoard::GetNetworkErrorText(const char* fallback) {
    return WifiStation::GetInstance().IsUserEnabled() ? fallback : LangString_WIFI_DISABLED_RETRY;
}

void WifiBoard::RefreshNetworkWaitDeadline(int timeout_ms) {
    if (WifiRadio::GetInstance().IsStationSetupActive()) {
        return;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    if (wifi_config_mode_) {
        return;
    }
    auto& wifi = WifiStation::GetInstance();
    if (wifi.IsConnected()) {
        return;
    }
    if (!WaitPanelThenRadioOk(timeout_ms > 0 ? timeout_ms : 50)) {
        ESP_LOGW(TAG, "RefreshNetworkWaitDeadline: defer radio (panel busy)");
        return;
    }
    EnsureWifiRadioUp(wifi);
    const int64_t dl = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    BumpEnsureDeadlineUs(dl);
    ESP_LOGI(TAG, "RefreshNetworkWaitDeadline +%d ms deadline_us=%" PRId64, timeout_ms, dl);
}

void WifiBoard::PauseNetworkIfNotReady() {
    if (WifiRadio::GetInstance().IsStationSetupActive()) {
        return;
    }
    if (wifi_config_mode_) {
        return;
    }
    auto& wifi = WifiStation::GetInstance();
    if (wifi.IsConnected()) {
        return;
    }
    ESP_LOGI(TAG, "PauseNetworkIfNotReady: pause STA (stop scan)");
    wifi.PauseForLp();
}

bool WifiBoard::IsNetworkReady() {
    return WifiStation::GetInstance().IsConnected();
}

bool WifiBoard::IsWifiConfigMode() {
    return wifi_config_mode_;
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

NetworkStatus WifiBoard::GetNetworkStatus() {
    NetworkStatus status;
    auto& station = WifiStation::GetInstance();
    if (wifi_config_mode_) {
        status.phase = NetworkPhase::Provisioning;
    } else if (station.IsLinkInProgress()) {
        status.phase = station.IsScanning() ? NetworkPhase::Scanning : NetworkPhase::Connecting;
    }
    wifi_ap_record_t ap_info{};
    if (!wifi_config_mode_ && station.TryGetApInfo(ap_info)) {
        status.phase = NetworkPhase::Ready;
        status.signal = ap_info.rssi;
    }
    return status;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // SoftAP 网页配网已移除；提示用户到设置里机上配置
    GetDisplay()->ShowNotification(LangString_SETTINGS_NET_WIFI_CFG_HINT, 5000);
}

std::string WifiBoard::GetDeviceStatusJson() try {
    const auto failed = []() {
        ESP_LOGE(TAG, "operation=device_status stage=encode result=no_memory");
        return std::string{};
    };
    auto& board = Board::GetInstance();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) {
        return failed();
    }
    auto* speaker = cJSON_AddObjectToObject(root.get(), "audio_speaker");
    auto* codec = board.GetAudioCodec();
    if (!speaker || (codec && !cJSON_AddNumberToObject(speaker, "volume", codec->output_volume()))) {
        return failed();
    }
    auto* screen = cJSON_AddObjectToObject(root.get(), "screen");
    auto* backlight = board.GetBacklight();
    if (!screen || (backlight && !cJSON_AddNumberToObject(screen, "brightness", backlight->brightness()))) {
        return failed();
    }
    auto* display = board.GetDisplay();
    if (display && display->height() > 64) {
        auto* theme = display->GetTheme();
        if (theme && !cJSON_AddStringToObject(screen, "theme", theme->name().c_str())) {
            return failed();
        }
    }
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto* battery = cJSON_AddObjectToObject(root.get(), "battery");
        if (!battery || !cJSON_AddNumberToObject(battery, "level", level) ||
            !cJSON_AddBoolToObject(battery, "charging", charging)) {
            return failed();
        }
    }
    auto* network = cJSON_AddObjectToObject(root.get(), "network");
    auto& station = WifiStation::GetInstance();
    wifi_ap_record_t ap_info = {};
    const bool available = station.TryGetApInfo(ap_info);
    const char* signal = "unavailable";
    if (available) {
        if (ap_info.rssi >= -60) {
            signal = "strong";
        } else if (ap_info.rssi >= -70) {
            signal = "medium";
        } else {
            signal = "weak";
        }
    }
    if (!network || !cJSON_AddStringToObject(network, "type", "wifi") ||
        !cJSON_AddStringToObject(network, "ssid", station.GetSsid().c_str()) ||
        !cJSON_AddStringToObject(network, "signal", signal)) {
        return failed();
    }
    float temperature = 0.0f;
    if (board.GetTemperature(temperature)) {
        auto* chip = cJSON_AddObjectToObject(root.get(), "chip");
        if (!chip || !cJSON_AddNumberToObject(chip, "temperature", temperature)) {
            return failed();
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!encoded) {
        return failed();
    }
    return std::string(encoded.get());
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=device_status stage=allocate result=no_memory");
    return {};
}
