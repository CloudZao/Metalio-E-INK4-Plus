#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "power_hw.h"
#include "power_policy.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <inttypes.h>

#include <algorithm>
#include <font_awesome.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include <esp_wifi.h>

static const char *TAG = "WifiBoard";

namespace {

portMUX_TYPE s_ensure_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_ensure_deadline_us = 0;
bool s_ensure_has_leader = false;

constexpr int kEnsureSliceMs = 200;
constexpr int kEnsureGiveUpPauseMs = 30000;  // 整段等网失败后 Pause STA
constexpr int64_t kEnsureWaitLogIntervalUs = 3 * 1000 * 1000;  // 未连上网时 enter/fail 日志限流
static int64_t s_last_ensure_wait_log_us = 0;

void BumpEnsureDeadlineUs(int64_t deadline_us) {
    portENTER_CRITICAL(&s_ensure_mux);
    if (deadline_us > s_ensure_deadline_us) {
        s_ensure_deadline_us = deadline_us;
    }
    portEXIT_CRITICAL(&s_ensure_mux);
}

/** STA 关/未起时拉起；已连上则 no-op */
void EnsureWifiRadioUp(WifiStation& wifi) {
    if (wifi.IsConnected()) {
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

/** 落墨中推迟起射频；timeout_ms 为最长等待，仍忙则 false */
bool WaitPanelThenRadioOk(int timeout_ms) {
    if (!Display::IsPanelBusy()) {
        return true;
    }
    const int wait_ms = timeout_ms <= 0 ? 50 : (timeout_ms > 2000 ? 2000 : timeout_ms);
    return Display::WaitPanelIdle(wait_ms);
}

}  // namespace

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

void WifiBoard::EnterWifiConfigMode() {
    // 网页 SoftAP 配网已移除：请到「设置 → 网络 → 配置 WIFI 网络」机上扫网配网。
    // 保留函数以免旧 force_ap / 板级 BOOT 路径崩掉；不再阻塞主启动。
    ESP_LOGW(TAG, "SoftAP web config removed; use Settings→Network WiFi setup");
    wifi_config_mode_ = false;
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(Lang::Strings::SETTINGS_NET_WIFI_CFG_HINT, 5000);
    }
}

void WifiBoard::StartNetwork() {
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
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 3000);
        display->UpdateStatusBar(true);
    });
    wifi_station.Start();

    // 有已存 WiFi：等保网时长拿 IP；失败不自动 SoftAP，Pause STA（停扫），下次 EnsureNetworkReady 再拉
    const int wait_ms = PowerPolicy::GetInstance().GetNetGraceSec() * 1000;
    if (!wifi_station.WaitForConnected(wait_ms)) {
        ESP_LOGW(TAG, "WiFi not connected within net_grace %d s; skip auto SoftAP, pause STA",
                 PowerPolicy::GetInstance().GetNetGraceSec());
        wifi_station.PauseForLp();
        return;
    }
}

bool WifiBoard::IsWifiConfigMode() {
    return wifi_config_mode_;
}

void WifiBoard::RefreshNetworkWaitDeadline(int timeout_ms) {
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

bool WifiBoard::EnsureNetworkReady(int timeout_ms) {
    auto& wifi = WifiStation::GetInstance();
    const char* task = pcTaskGetName(nullptr);
    const int core = xPortGetCoreID();
    const UBaseType_t stack_before = uxTaskGetStackHighWaterMark(nullptr);
    const int64_t t0 = esp_timer_get_time();
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    const bool already = wifi.IsConnected();
    // 百问等短切片每 200ms 进一次：未连上时 enter/fail 最多 3s 打一对，避免刷屏
    const bool log_wait =
        !already && (t0 - s_last_ensure_wait_log_us >= kEnsureWaitLogIntervalUs);
    if (already || log_wait) {
        ESP_LOGI(TAG,
                 "EnsureNetworkReady: enter task=%s core=%d timeout_ms=%d stack_hwm=%u connected=%d",
                 task != nullptr ? task : "?", core, timeout_ms, static_cast<unsigned>(stack_before),
                 already ? 1 : 0);
    }
    if (already) {
        return true;
    }
    // SoftAP 配网无 STA 互联网，空等只会拖死主循环/业务线程
    if (wifi_config_mode_) {
        ESP_LOGW(TAG, "EnsureNetworkReady: skip (wifi config mode) task=%s",
                 task != nullptr ? task : "?");
        return false;
    }

    // 墨水落墨中勿起/扫射频：流式行队列缓冲短，WiFi bring-up 易 underrun
    if (!WaitPanelThenRadioOk(timeout_ms)) {
        if (log_wait) {
            ESP_LOGW(TAG, "EnsureNetworkReady: defer radio (panel busy) task=%s",
                     task != nullptr ? task : "?");
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
    while (!wifi.IsConnected()) {
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
            while (!wifi.IsConnected()) {
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
                 static_cast<int>((esp_timer_get_time() - t0) / 1000),
                 static_cast<unsigned>(stack_before), static_cast<unsigned>(stack_after));
        return true;
    }
    if (log_wait) {
        ESP_LOGW(TAG,
                 "EnsureNetworkReady: WiFi not connected within %d ms task=%s core=%d "
                 "elapsed_ms=%d stack_hwm=%u->%u",
                 timeout_ms, task != nullptr ? task : "?", core,
                 static_cast<int>((esp_timer_get_time() - t0) / 1000),
                 static_cast<unsigned>(stack_before), static_cast<unsigned>(stack_after));
        s_last_ensure_wait_log_us = esp_timer_get_time();
    }
    // 整段等网失败：停扫省电；短切片（百问/翻译）由预算循环结束时 PauseNetworkIfNotReady
    if (timeout_ms >= kEnsureGiveUpPauseMs) {
        ESP_LOGI(TAG, "EnsureNetworkReady give-up (>=%d ms) -> Pause STA", kEnsureGiveUpPauseMs);
        wifi.PauseForLp();
    }
    return false;
}

bool WifiBoard::IsNetworkReady() {
    return WifiStation::GetInstance().IsConnected();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (wifi_station.IsConnected()) {
        int8_t rssi = wifi_station.GetRssi();
        if (rssi >= -60) {
            return FONT_AWESOME_WIFI;
        } else if (rssi >= -70) {
            return FONT_AWESOME_WIFI_FAIR;
        }
        return FONT_AWESOME_WIFI_WEAK;
    }
    // 扫描/连接中：图标固定未连接，进度改由顶栏文案表示
    return FONT_AWESOME_WIFI_SLASH;
}

const char* WifiBoard::GetNetworkBusyStatusText() {
    if (wifi_config_mode_) {
        return nullptr;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsLinkInProgress()) {
        return nullptr;
    }
    return wifi_station.IsScanning() ? Lang::Strings::WIFI_STATUS_SCANNING
                                     : Lang::Strings::WIFI_STATUS_CONNECTING;
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // SoftAP 网页配网已移除；提示用户到设置里机上配置
    GetDisplay()->ShowNotification(Lang::Strings::SETTINGS_NET_WIFI_CFG_HINT, 5000);
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * Return device status JSON
     * 
     * The returned JSON structure is as follows:
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
