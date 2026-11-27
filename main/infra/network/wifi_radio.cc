#include <cstdio>
#include <cstring>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <wifi_station.h>
#include <lwip/ip4_addr.h>
#include "wifi_radio.h"
#include "config.h"

static const char* TAG = "WifiRadio";

static esp_err_t WifiRadioFailure(const char* stage, esp_err_t error) {
    ESP_LOGE(TAG, "operation=radio stage=%s code=%d", stage, static_cast<int>(error));
    return error;
}

WifiRadio& WifiRadio::GetInstance() {
    static WifiRadio instance;
    return instance;
}

WifiApStartResult WifiRadio::StartAccessPoint(const char* ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ssid || !*ssid || std::strlen(ssid) > 32 || sleeping_ || station_setup_ ||
        !WifiStation::GetInstance().IsUserEnabled()) {
        return WifiApStartResult::InitFailed;
    }
    if (stop_pending_ && StopAccessPointLocked(true) != ESP_OK) {
        return WifiApStartResult::StartFailed;
    }
    if (ap_owned_) {
        if (ap_started_) {
            return WifiApStartResult::Ok;
        }
        if (StopAccessPointLocked(true) != ESP_OK) {
            return WifiApStartResult::StartFailed;
        }
    }
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        WifiRadioFailure("netif_init", error);
        return WifiApStartResult::InitFailed;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        WifiRadioFailure("event_loop", error);
        return WifiApStartResult::InitFailed;
    }
    if (!ap_netif_) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
        if (!ap_netif_) {
            WifiRadioFailure("ap_netif", ESP_ERR_NO_MEM);
            return WifiApStartResult::InitFailed;
        }
    }
    esp_netif_ip_info_t ip{};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    error = esp_netif_dhcps_stop(ap_netif_);
    if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        WifiRadioFailure("dhcp_stop", error);
        return WifiApStartResult::InitFailed;
    }
    error = esp_netif_set_ip_info(ap_netif_, &ip);
    if (error == ESP_OK) {
        error = esp_netif_dhcps_start(ap_netif_);
    }
    if (error != ESP_OK) {
        WifiRadioFailure("dhcp_start", error);
        return WifiApStartResult::InitFailed;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = false;
        error = esp_wifi_init(&config);
        if (error != ESP_OK) {
            WifiRadioFailure("driver_init", error);
            return WifiApStartResult::InitFailed;
        }
        driver_owned_ = true;
    } else {
        if ((mode & WIFI_MODE_AP) != 0) {
            return WifiApStartResult::InitFailed;
        }
        previous_mode_ = static_cast<int>(mode);
        WifiStation::GetInstance().PauseForLp();
        station_paused_ = WifiStation::GetInstance().IsLpPaused();
    }
    ap_owned_ = true;
    wifi_config_t config{};
    std::memcpy(config.ap.ssid, ssid, std::strlen(ssid));
    config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    error = esp_wifi_set_mode(WIFI_MODE_AP);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    #if BOARD_WIFI_MAX_TX_POWER > 0
    if (error == ESP_OK) {
        error = esp_wifi_set_max_tx_power(BOARD_WIFI_MAX_TX_POWER);
    }
    #endif
    if (error != ESP_OK) {
        WifiRadioFailure("ap_start", error);
        StopAccessPointLocked(true);
        return WifiApStartResult::StartFailed;
    }
    ap_started_ = true;
    ESP_LOGI(TAG, "operation=ap_start result=success");
    return WifiApStartResult::Ok;
}

esp_err_t WifiRadio::StopAccessPointLocked(bool resume_station) {
    stop_pending_ = true;
    if (ap_owned_) {
        const auto error = esp_wifi_stop();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED && error != ESP_ERR_WIFI_NOT_INIT) {
            return WifiRadioFailure("ap_stop", error);
        }
        ap_started_ = false;
        if (driver_owned_) {
            const auto error = esp_wifi_deinit();
            if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
                return WifiRadioFailure("driver_deinit", error);
            }
            driver_owned_ = false;
        } else {
            const auto error = esp_wifi_set_mode(static_cast<wifi_mode_t>(previous_mode_));
            if (error != ESP_OK) {
                return WifiRadioFailure("station_mode", error);
            }
        }
        ap_owned_ = false;
    }
    if (station_paused_ && resume_station && !sleeping_ && WifiStation::GetInstance().IsUserEnabled()) {
        const auto error = WifiStation::GetInstance().ResumeFromLp();
        if (error != ESP_OK) {
            return WifiRadioFailure("station_resume", error);
        }
        station_paused_ = false;
    }
    stop_pending_ = false;
    return ESP_OK;
}

esp_err_t WifiRadio::StopAccessPoint(bool resume_station) {
    std::lock_guard<std::mutex> lock(mutex_);
    return StopAccessPointLocked(resume_station);
}

esp_err_t WifiRadio::PauseForSleep(bool standby) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station_setup_) {
        return ESP_ERR_INVALID_STATE;
    }
    sleeping_ = standby;
    if (stop_pending_) {
        const auto error = StopAccessPointLocked(false);
        if (error != ESP_OK) {
            return error;
        }
    }
    if (ap_owned_) {
        return ESP_ERR_INVALID_STATE;
    }
    WifiStation::GetInstance().PauseForLp();
    return ESP_OK;
}

esp_err_t WifiRadio::ResumeFromSleep() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station_setup_) {
        return ESP_ERR_INVALID_STATE;
    }
    sleeping_ = false;
    auto& station = WifiStation::GetInstance();
    if (ap_owned_ || !station.IsUserEnabled()) {
        return ESP_OK;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool ready = esp_wifi_get_mode(&mode) == ESP_OK;
    if (ready && (mode & WIFI_MODE_AP)) {
        return ESP_OK;
    }
    if (station.IsLpPaused()) {
        const auto error = station.ResumeFromLp();
        if (error != ESP_OK) {
            WifiRadioFailure("sleep_resume", error);
            station.Start();
            return error;
        }
        station_paused_ = false;
    } else if (!ready || mode == WIFI_MODE_NULL) {
        station.Start();
    }
    return ESP_OK;
}

bool WifiRadio::BeginStationSetup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station_setup_ || sleeping_ || ap_owned_ || stop_pending_) {
        return false;
    }
    station_setup_ = true;
    return true;
}

void WifiRadio::EndStationSetup() {
    std::lock_guard<std::mutex> lock(mutex_);
    station_setup_ = false;
}

bool WifiRadio::IsStationSetupActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_setup_;
}
