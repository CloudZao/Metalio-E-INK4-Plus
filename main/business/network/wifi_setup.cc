#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_station.h>
#include "wifi_setup.h"
#include "network_settings.h"
#include "infra/tasks/task_create.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "assets/lang_screen.h"
#include "boards/common/board/dual_network_board.h"
#include "infra/network/wifi_radio.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "sysmgmt/system_control.h"

static constexpr const char* TAG = "WifiSetup";
static constexpr uint32_t kStackBytes = 8192;
static constexpr EventBits_t kScanDone = BIT0;
static constexpr EventBits_t kConnected = BIT1;
static constexpr EventBits_t kDisconnected = BIT2;

struct WifiSetupWork {
    WifiSetupRequest result;
    std::string password;
};

struct WifiSetupSession {
    explicit WifiSetupSession(bool local_transfer) : transfer(local_transfer) {}
    std::mutex mutex;
    const bool transfer;
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> resume_station{true};
    std::atomic<uint8_t> disconnect_reason{0};
    bool busy = false;
    TaskHandle_t task = nullptr;
    std::unique_ptr<WifiSetupWork> pending;
    EventGroupHandle_t events = nullptr; // UI取消与事件回调在mutex内借用，注销后才销毁
    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler = nullptr;
    esp_netif_t* netif = nullptr;
    bool driver_initialized = false;
    bool driver_ready = false;
    bool station_was_active = false;
    bool radio_reserved = false;
    std::optional<PowerNeedHold> network_hold;
};

static std::mutex s_mutex;
static std::weak_ptr<WifiSetupSession> s_session;

static const char* UnavailableReason() {
    if (NetworkSettingsCurrentType() != NetworkType::WIFI) {
        return LangString_CLOUD_CELLULAR_NEED_WIFI;
    }
    return WifiStation::GetInstance().IsUserEnabled() ? nullptr : LangString_WIFI_DISABLED_RETRY;
}

static bool SetupFailure(const char* stage, esp_err_t error) {
    ESP_LOGW(TAG, "operation=driver stage=%s error=%d", stage, int(error));
    return false;
}

static void OnWifiEvent(void* context, esp_event_base_t base, int32_t id, void* data) {
    auto& session = *static_cast<WifiSetupSession*>(context);
    EventBits_t bits = 0;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            bits = kScanDone;
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
            session.disconnect_reason.store(event ? event->reason : 0);
            bits = kDisconnected;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        bits = kConnected;
    }
    if (bits) {
        std::lock_guard<std::mutex> lock(session.mutex);
        if (session.events) {
            xEventGroupSetBits(session.events, bits);
        }
    }
}

static bool TearDownDriver(WifiSetupSession& session, bool diagnose = true) {
    // 注销在事件锁外等待回调退出，避免与回调获取同一把锁互相等待。
    if (session.wifi_handler) {
        const auto error = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, session.wifi_handler);
        if (error != ESP_OK) {
            return diagnose ? SetupFailure("unregister_wifi", error) : false;
        }
        session.wifi_handler = nullptr;
    }
    if (session.ip_handler) {
        const auto error = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, session.ip_handler);
        if (error != ESP_OK) {
            return diagnose ? SetupFailure("unregister_ip", error) : false;
        }
        session.ip_handler = nullptr;
    }
    if (session.driver_initialized) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        const auto stopped = esp_wifi_stop();
        if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_STARTED && stopped != ESP_ERR_WIFI_NOT_INIT) {
            return diagnose ? SetupFailure("stop", stopped) : false;
        }
        const auto error = esp_wifi_deinit();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
            return diagnose ? SetupFailure("deinit", error) : false;
        }
        session.driver_initialized = false;
    }
    if (session.netif) {
        esp_netif_destroy_default_wifi(session.netif);
        session.netif = nullptr;
    }
    session.driver_ready = false;
    return true;
}

static bool EnsureDriver(WifiSetupSession& session) {
    if (UnavailableReason() || session.closing.load()) {
        return false;
    }
    if (session.driver_ready) {
        return true;
    }
    if (!session.radio_reserved) {
        session.radio_reserved = WifiRadio::GetInstance().BeginStationSetup();
        if (!session.radio_reserved) {
            return SetupFailure("radio_owned", ESP_ERR_INVALID_STATE);
        }
    }
    if (!TearDownDriver(session)) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_NULL) {
        session.station_was_active = true;
        WifiStation::GetInstance().Stop();
    }
    if (session.closing.load()) {
        return false;
    }
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return SetupFailure("netif_init", error);
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return SetupFailure("event_loop", error);
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        return SetupFailure("foreign_station", ESP_ERR_INVALID_STATE);
    }
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        if (!session.events) {
            session.events = xEventGroupCreate();
        }
    }
    if (!session.events) {
        return SetupFailure("event_group", ESP_ERR_NO_MEM);
    }
    session.netif = esp_netif_create_default_wifi_sta();
    if (!session.netif) {
        return SetupFailure("create_station", ESP_ERR_NO_MEM);
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = false;
    error = esp_wifi_init(&config);
    if (error != ESP_OK) {
        TearDownDriver(session);
        return SetupFailure("init", error);
    }
    session.driver_initialized = true;
    error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, OnWifiEvent, &session, &session.wifi_handler);
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnWifiEvent, &session, &session.ip_handler);
    }
    if (error != ESP_OK) {
        TearDownDriver(session);
        return SetupFailure("register_events", error);
    }
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        TearDownDriver(session);
        return SetupFailure("start", error);
    }
    session.driver_ready = true;
    ESP_LOGI(TAG, "operation=driver result=ready");
    return true;
}

static void Scan(WifiSetupSession& session, WifiSetupResult& result) {
    result.stage.store(WifiSetupStage::Scanning, std::memory_order_release);
    xEventGroupClearBits(session.events, kScanDone);
    if (session.closing.load()) {
        return;
    }
    wifi_scan_config_t config{};
    const auto error = esp_wifi_scan_start(&config, false);
    if (error != ESP_OK) {
        SetupFailure("scan_start", error);
        return;
    }
    const auto bits = xEventGroupWaitBits(session.events, kScanDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(15000));
    if (session.closing.load() || !(bits & kScanDone)) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        result.timeout = !session.closing.load();
        return;
    }
    struct ScanRecordsGuard {
        ~ScanRecordsGuard() { esp_wifi_clear_ap_list(); }
    } records_guard;
    uint16_t count = 0;
    const auto count_error = esp_wifi_scan_get_ap_num(&count);
    if (count_error != ESP_OK) {
        SetupFailure("scan_count", count_error);
        return;
    }
    count = std::min<uint16_t>(count, 64);
    std::vector<wifi_ap_record_t> records(count);
    if (count) {
        const auto error = esp_wifi_scan_get_ap_records(&count, records.data());
        if (error != ESP_OK) {
            SetupFailure("scan_records", error);
            return;
        }
    }
    std::sort(records.begin(), records.begin() + count, [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    for (size_t i = 0; i < count; ++i) {
        char ssid[33];
        std::memcpy(ssid, records[i].ssid, 32);
        ssid[32] = '\0';
        if (!ssid[0] || std::any_of(result.access_points.begin(), result.access_points.end(),
                                  [ssid](const auto& item) { return item.ssid == ssid; })) {
            continue;
        }
        result.access_points.push_back({ssid, records[i].rssi, records[i].authmode});
    }
    result.ok = true;
}

static void Connect(WifiSetupSession& session, WifiSetupWork& work) {
    auto& result = *work.result;
    result.stage.store(WifiSetupStage::Connecting, std::memory_order_release);
    if (session.transfer) {
        ApTransferStop();
    }
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, result.ssid.data(), result.ssid.size());
    std::memcpy(config.sta.password, work.password.data(), work.password.size());
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.failure_retry_cnt = 1;
    xEventGroupClearBits(session.events, kConnected | kDisconnected);
    session.disconnect_reason.store(0);
    if (session.closing.load()) {
        return;
    }
    auto error = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (error == ESP_OK) {
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        SetupFailure("connect_start", error);
        return;
    }
    const auto bits = xEventGroupWaitBits(session.events, kConnected | kDisconnected, pdTRUE, pdFALSE,
                                         pdMS_TO_TICKS(15000));
    if (session.closing.load()) {
        return;
    }
    if (bits & kConnected) {
        SsidManager::GetInstance().AddSsid(result.ssid, work.password);
        const auto saved = SsidManager::GetInstance().GetSsidList();
        const bool committed = std::any_of(saved.begin(), saved.end(), [&](const auto& item) {
            return item.ssid == result.ssid && item.password == work.password;
        });
        if (!committed) {
            std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
            return;
        }
        result.ok = !session.transfer || WifiTransferStart(&result.transfer);
        if (!result.ok) {
            std::snprintf(result.error, sizeof(result.error), "%s",
                          result.transfer.fail ? result.transfer.fail : LangString_CLOUD_AP_FAIL_HTTP);
        }
    } else if (bits & kDisconnected) {
        result.disconnect_reason = session.disconnect_reason.load();
    } else {
        esp_wifi_disconnect();
        result.timeout = true;
    }
}

static void ChangeSavedNetwork(WifiSetupResult& result) {
    auto& manager = SsidManager::GetInstance();
    if (result.operation == WifiSetupOperation::SetDefault) {
        manager.SetDefaultSsid(result.ssid);
    } else if (result.operation == WifiSetupOperation::Remove) {
        manager.RemoveSsid(result.ssid);
    } else {
        manager.Clear();
    }
    const auto saved = manager.GetSsidList();
    result.ok = result.operation == WifiSetupOperation::Clear ? saved.empty()
                : result.operation == WifiSetupOperation::SetDefault ? !saved.empty() && saved[0].ssid == result.ssid
                : std::none_of(saved.begin(), saved.end(), [&](const auto& item) { return item.ssid == result.ssid; });
    if (!result.ok) {
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}

static void RunSession(void* context) {
    auto owner = std::unique_ptr<WifiSetupSessionPtr>(static_cast<WifiSetupSessionPtr*>(context));
    auto& session = **owner;
    for (;;) {
        std::unique_ptr<WifiSetupWork> work;
        {
            std::lock_guard<std::mutex> lock(session.mutex);
            work = std::move(session.pending);
        }
        if (work) {
            const auto result = work->result;
            {
                PowerNeedHold hold(PowerNeed::StandbyInhibit);
                try {
                    if (!session.closing.load()) {
                        if (result->operation == WifiSetupOperation::Scan || result->operation == WifiSetupOperation::Connect) {
                            result->stage.store(WifiSetupStage::Initializing, std::memory_order_release);
                            if (EnsureDriver(session)) {
                                if (result->operation == WifiSetupOperation::Scan) {
                                    Scan(session, *result);
                                } else {
                                    Connect(session, *work);
                                }
                            } else {
                                const char* reason = UnavailableReason();
                                std::snprintf(result->error, sizeof(result->error), "%s",
                                              reason ? reason : LangString_NETWORK_WIFI_INIT_FAIL);
                            }
                        } else {
                            ChangeSavedNetwork(*result);
                        }
                    }
                } catch (const std::bad_alloc&) {
                    result->ok = false;
                    std::snprintf(result->error, sizeof(result->error), "%s", LangString_COMMON_MEMORY_RETRY);
                    ESP_LOGE(TAG, "operation=execute kind=%u result=no_memory", unsigned(result->operation));
                }
                work.reset();
            }
            if (result->ok && result->operation == WifiSetupOperation::Connect && !session.transfer) {
                PowerNeedHold hold(PowerNeed::StandbyInhibit);
                result->reboot_at_us.store(esp_timer_get_time() + 3200000);
                result->stage.store(WifiSetupStage::Restart, std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(3200));
                SystemRequestReboot();
            }
            result->completed.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                session.busy = false;
            }
        }
        if (session.closing.load()) {
            std::unique_ptr<WifiSetupWork> pending;
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                pending = std::move(session.pending);
                session.busy = false;
            }
            if (pending) {
                const auto result = pending->result;
                pending.reset();
                result->completed.store(true, std::memory_order_release);
            }
            break;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    {
        PowerNeedHold hold(PowerNeed::StandbyInhibit);
        if (session.transfer) {
            ApTransferRequestStop();
            ApTransferStop();
        }
        bool retrying = false;
        while (!TearDownDriver(session, !retrying)) {
            retrying = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (retrying) {
            ESP_LOGI(TAG, "operation=driver stage=close result=recovered");
        }
        {
            std::lock_guard<std::mutex> lock(session.mutex);
            if (session.events) {
                vEventGroupDelete(session.events);
                session.events = nullptr;
            }
        }
        if (session.station_was_active && session.resume_station.load() &&
            !PowerLifecycleIsStandby() && WifiStation::GetInstance().IsUserEnabled()) {
            WifiStation::GetInstance().Start();
        }
        if (session.radio_reserved) {
            WifiRadio::GetInstance().EndStationSetup();
            session.radio_reserved = false;
        }
    }
    session.network_hold.reset();
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.task = nullptr;
        session.closed.store(true, std::memory_order_release);
    }
    owner.reset();
    vTaskDelete(nullptr);
}

WifiSetupSessionPtr WifiSetupOpen(bool transfer) try {
    auto session = std::make_shared<WifiSetupSession>(transfer);
    auto owner = std::make_unique<WifiSetupSessionPtr>(session);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const auto previous = s_session.lock();
        if (previous && !previous->closed.load(std::memory_order_acquire)) {
            return {};
        }
        s_session = session;
    }
    // 保网可能同步恢复射频并查询当前会话，不得在会话注册锁内触发电源回调。
    session->network_hold.emplace(PowerNeed::UiKeepNet);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (FirmwareCreateTask(RunSession, "wifi_setup", kStackBytes, owner.get(), 3, &session->task, tskNO_AFFINITY, 0) != pdPASS) {
            session->closed.store(true, std::memory_order_release);
            ESP_LOGW(TAG, "operation=open stage=create_task result=unavailable");
            return {};
        }
    }
    owner.release();
    return session;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=open stage=allocate result=no_memory");
    return {};
}

WifiSetupRequest WifiSetupSubmit(const WifiSetupSessionPtr& session, WifiSetupOperation operation,
                                const std::string& ssid, const std::string& password) try {
    if (!session || operation < WifiSetupOperation::Scan || operation > WifiSetupOperation::Clear ||
        ssid.size() > 32 || password.size() > 64 || ssid.find('\0') != std::string::npos ||
        password.find('\0') != std::string::npos ||
        (operation != WifiSetupOperation::Scan && operation != WifiSetupOperation::Clear && ssid.empty())) {
        return {};
    }
    auto result = std::make_shared<WifiSetupResult>();
    result->operation = operation;
    result->ssid = ssid;
    auto work = std::make_unique<WifiSetupWork>(WifiSetupWork{result, password});
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->closing.load() || session->busy || !session->task) {
        return {};
    }
    session->busy = true;
    session->pending = std::move(work);
    xTaskNotifyGive(session->task);
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=submit stage=allocate result=no_memory");
    return {};
}

void WifiSetupClose(const WifiSetupSessionPtr& session, bool resume_station) {
    if (!session) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->closing.store(true);
    session->resume_station.store(resume_station);
    if (session->events) {
        xEventGroupSetBits(session->events, kScanDone | kConnected | kDisconnected);
    }
    if (session->task) {
        xTaskNotifyGive(session->task);
    }
}

void WifiSetupCloseCurrent(bool resume_station) {
    WifiSetupSessionPtr session;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        session = s_session.lock();
    }
    WifiSetupClose(session, resume_station);
}

bool WifiSetupIsClosed() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto session = s_session.lock();
    return !session || session->closed.load(std::memory_order_acquire);
}

bool WifiSetupIsOpen(const WifiSetupSessionPtr& session) {
    return session && !session->closing.load() && !session->closed.load();
}
