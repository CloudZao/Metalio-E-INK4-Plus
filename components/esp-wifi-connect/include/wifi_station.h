#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>
#include <sdkconfig.h>

#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
constexpr int8_t kMetalioWifiMaxTxPower = 48; // TX 功率请求上限，单位 0.25 dBm，对应 12 dBm
#endif

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

class WifiStation {
public:
    static WifiStation& GetInstance();
    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    /** @brief 设置并保存 Wi-Fi 用户开关；关闭后禁止后台自动恢复，保存失败时不切换 */
    esp_err_t SetUserEnabled(bool enabled);
    /** @brief 查询用户是否允许启用 Wi-Fi，重启后恢复上次设置，未设置时允许 */
    bool IsUserEnabled() const { return user_enabled_.load(); }
    /**
     * @brief 待机浅睡：只停射频，保留 netif/事件，避免反复 destroy/create
     */
    void PauseForLp();
    /**
     * @brief 待机唤醒：恢复射频并重连
     * @return ESP_OK 成功
     */
    esp_err_t ResumeFromLp();
    /**
     * @brief 是否处于浅睡暂停态
     */
    bool IsLpPaused() const { return lp_paused_; }
    bool IsConnected();
    /**
     * @brief STA 已起且未连上（扫描/关联/重连中）；浅睡暂停时为 false
     */
    bool IsLinkInProgress();
    /**
     * @brief 未连上且处于扫网阶段（相对关联/重连）
     */
    bool IsScanning();
    bool WaitForConnected(int timeout_ms = 10000);
    // Status queries tolerate disconnect/stop racing with the driver call.
    // On failure, ap_info is cleared and callers must treat it as unavailable.
    bool TryGetApInfo(wifi_ap_record_t& ap_info);
    static constexpr int8_t kRssiUnavailable = -128;
    static constexpr uint8_t kChannelUnavailable = 0;
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveMode(bool enabled);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnScanBegin(std::function<void()> on_scan_begin);

private:
    WifiStation();
    ~WifiStation();
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    esp_netif_t* station_netif_ = nullptr;
    std::atomic<bool> lp_paused_{false}; // 跨任务阻止暂停或停止后的后台连接
    std::atomic<bool> user_enabled_{true}; // 用户手动关闭优先于自动联网及待机恢复
    bool fast_reconnect_on_start_ = false; // LP resume：STA_START 直 connect，跳过扫网
    bool scanning_ = false; // 扫网中（未连上时相对关联/重连）
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    int reconnect_count_ = 0;
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void()> on_scan_begin_;
    std::vector<WifiApRecord> connect_queue_;

    void HandleScanResult();
    void StartConnect();
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
