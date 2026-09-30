#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"
#include "wifi_scan_lease.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5
// esp_timer 单位为微秒；扫不到已存 AP 时隔多久再扫（勿写成 10*1000=10ms）
constexpr uint64_t kScanRetryUs = 10ULL * 1000 * 1000;

static esp_err_t NetifInitOnce()
{
    esp_err_t err = esp_netif_init();
    return (err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
    }
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    lp_paused_ = false;
    fast_reconnect_on_start_ = false;
    scanning_ = false;
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    esp_wifi_scan_stop();
    
    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_));
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_));
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    if (station_netif_ != nullptr) {
        esp_netif_destroy(station_netif_);
        station_netif_ = nullptr;
    }

    // Clear event group bits to prevent WaitForConnected from returning prematurely on restart
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
}

void WifiStation::PauseForLp() {
    if (lp_paused_) {
        return;
    }
    // 未持有 STA（如网络页已 Stop）时勿 stop 他人射频
    if (station_netif_ == nullptr) {
        return;
    }
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }
    esp_wifi_scan_stop();
    scanning_ = false;
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    // 先置位，避免 esp_wifi_stop 触发的 DISCONNECTED 里再 connect/扫网
    lp_paused_ = true;
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LP pause wifi stop: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "STA paused for LP (radio off, netif kept)");
}

esp_err_t WifiStation::ResumeFromLp() {
    if (!lp_paused_) {
        return ESP_OK;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGW(TAG, "LP resume skipped (wifi not init)");
        lp_paused_ = false;
        return ESP_ERR_INVALID_STATE;
    }
    reconnect_count_ = 0;
    // 须先清 pause：esp_wifi_start 可能同步抛 STA_START，否则扫网被丢掉
    lp_paused_ = false;
    fast_reconnect_on_start_ = !ssid_.empty();
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        lp_paused_ = true;
        fast_reconnect_on_start_ = false;
        ESP_LOGE(TAG, "LP resume wifi start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "STA resumed from LP");
    return ESP_OK;
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    if (station_netif_ != nullptr) {
        if (lp_paused_) {
            ResumeFromLp();
        } else {
            ESP_LOGW(TAG, "Start skipped (already inited)");
        }
        return;
    }

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(NetifInitOnce());

    // Create the default event loop
    station_netif_ = esp_netif_create_default_wifi_sta();

    // Initialize the WiFi stack in station mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<WifiStation*>(arg);
            if (self->lp_paused_ || self->timer_handle_ == nullptr) {
                return;
            }
            self->scanning_ = true;
            esp_wifi_scan_start(nullptr, false);
            if (self->on_scan_begin_) {
                self->on_scan_begin_();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan");
        if (timer_handle_ != nullptr && !lp_paused_) {
            esp_timer_start_once(timer_handle_, kScanRetryUs);
        }
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;
    scanning_ = false;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

bool WifiStation::IsLinkInProgress() {
    if (lp_paused_ || station_netif_ == nullptr) {
        return false;
    }
    return !IsConnected();
}

bool WifiStation::IsScanning() {
    return IsLinkInProgress() && scanning_;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    ESP_ERROR_CHECK(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (this_->lp_paused_) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        }
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        if (this_->fast_reconnect_on_start_) {
            this_->fast_reconnect_on_start_ = false;
            wifi_config_t cfg = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
                this_->reconnect_count_ = 0;
                this_->scanning_ = false;
                if (this_->on_connect_ && !this_->ssid_.empty()) {
                    this_->on_connect_(this_->ssid_);
                }
                esp_wifi_connect();
                return;
            }
        }
        this_->scanning_ = true;
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        // 已连上：外部被动扫网（如指纹定位）勿抢表、勿 StartConnect
        if (this_->IsConnected()) {
            return;
        }
        // 租约持有中：把 AP 表留给持有方；稍后再扫，避免重连扫网断档
        if (WifiScanLease::IsHeld()) {
            if (this_->timer_handle_ != nullptr && !this_->lp_paused_) {
                esp_timer_start_once(this_->timer_handle_, kScanRetryUs);
            }
            return;
        }
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            this_->scanning_ = false;
            if (this_->on_connect_ && !this_->ssid_.empty()) {
                this_->on_connect_(this_->ssid_);
            }
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        if (this_->timer_handle_ != nullptr && !this_->lp_paused_) {
            esp_timer_start_once(this_->timer_handle_, kScanRetryUs);
        }
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->scanning_ = false;
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}
