#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <wifi_scan_lease.h>
#include <wifi_station.h>
#include "infra/tasks/task_create.h"
#include "infra/network/wifi_radio.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "wifi_diagnostic.h"

static constexpr const char* kTag = "WifiDiagnostic";
static constexpr const char* kLeaseOwner = "settings_test";
static constexpr unsigned kStackBytes = 6144;
static std::atomic<uint32_t> s_scan_generation{0};
static thread_local uint32_t s_worker_generation = 0;
static std::mutex s_mutex;
static std::weak_ptr<WifiDiagnosticResult> s_request;
static bool s_running = false;
static EventGroupHandle_t s_events = nullptr; // s_mutex内借用，注销回调后才销毁
struct WifiDiagnosticDriver {
    bool wifi_stack_owned = false;
    esp_netif_t* wifi_netif = nullptr;
};
static WifiDiagnosticDriver s_driver;
static WifiDiagnosticDriver& WifiDiagnostic_State() { return s_driver; }
static bool WifiDiagnostic_IsLive() { return s_worker_generation == s_scan_generation.load(); }

class WifiLifecycleLock {
public:
    WifiLifecycleLock() {
        static StaticSemaphore_t storage;
        static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
        mutex_ = mutex;
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }

    ~WifiLifecycleLock() {
        xSemaphoreGive(mutex_);
    }

private:
    SemaphoreHandle_t mutex_;
};

static bool WifiTeardownLocked() {
    auto& ui = WifiDiagnostic_State();
    if (ui.wifi_stack_owned) {
        esp_wifi_stop();
        const esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            // Retain ownership so a later teardown can retry safely.
            ESP_LOGE(kTag, "esp_wifi_deinit: %s", esp_err_to_name(err));
            return false;
        }
        ui.wifi_stack_owned = false;
    }
    // The netif may exist even when esp_wifi_init failed before ownership
    // of the driver was acquired. Also detach its default driver/handlers.
    if (ui.wifi_netif != nullptr) {
        esp_netif_destroy_default_wifi(ui.wifi_netif);
        ui.wifi_netif = nullptr;
    }
    return true;
}

static bool WifiEnsureForScan() {
    if (!WifiStation::GetInstance().IsUserEnabled()) {
        return false;
    }
    WifiLifecycleLock lock;
    // Teardown may have invalidated this worker while it waited for the lock.
    if (!WifiDiagnostic_IsLive() || s_worker_generation != s_scan_generation.load()) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode);
    if (mode_err == ESP_OK && mode != WIFI_MODE_NULL) {
        const esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "esp_wifi_start: %s", esp_err_to_name(start_err));
            WifiTeardownLocked();
            return false;
        }
        return true;
    }

    // An initialized NULL-mode driver belongs to another client; do not
    // reinitialize it or destroy that client's default STA on failure.
    if (mode_err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "WiFi unavailable for temporary scan: mode=%d err=%s", static_cast<int>(mode),
                 esp_err_to_name(mode_err));
        return false;
    }
    if (!WifiTeardownLocked()) {
        return false;
    }

    // 4G / WiFi 未起：临时拉 STA 只扫，扫完 Teardown 卸栈
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "event_loop: %s", esp_err_to_name(err));
        return false;
    }

    if (WifiDiagnostic_State().wifi_netif == nullptr) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
            ESP_LOGW(kTag, "default STA already belongs to another client");
            return false;
        }
        WifiDiagnostic_State().wifi_netif = esp_netif_create_default_wifi_sta();
    }
    if (WifiDiagnostic_State().wifi_netif == nullptr) {
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_init: %s", esp_err_to_name(err));
        WifiTeardownLocked();
        return false;
    }
    WifiDiagnostic_State().wifi_stack_owned = true;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        WifiTeardownLocked();
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_wifi_start: %s", esp_err_to_name(err));
        WifiTeardownLocked();
        return false;
    }
    return true;
}


struct WifiDiagnosticScan {
    std::vector<wifi_ap_record_t> records;
    EventGroupHandle_t events = nullptr;
    std::mutex mutex;
    bool captured = false;
    bool success = false;
};

static void OnScanDone(void* context, esp_event_base_t, int32_t, void* data) {
    if (data && static_cast<wifi_event_sta_scan_done_t*>(data)->status != 0) {
        return;
    }
    auto& scan = *static_cast<WifiDiagnosticScan*>(context);
    std::lock_guard<std::mutex> lock(scan.mutex);
    if (scan.captured) {
        return;
    }
    uint16_t count = 0;
    auto error = esp_wifi_scan_get_ap_num(&count);
    count = std::min<uint16_t>(count, 64);
    if (error == ESP_OK && count) {
        error = esp_wifi_scan_get_ap_records(&count, scan.records.data());
    }
    scan.records.resize(error == ESP_OK ? count : 0);
    scan.success = error == ESP_OK;
    scan.captured = true;
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "operation=scan stage=capture error=%d", int(error));
    }
    xEventGroupSetBits(scan.events, 1);
}

static void RunScan(WifiDiagnosticResult& result) {
    if (!WifiScanLease::TryAcquire(kLeaseOwner)) {
        ESP_LOGW(kTag, "operation=scan stage=lease result=busy");
        return;
    }
    struct LeaseGuard { ~LeaseGuard() { WifiScanLease::Release(); } } lease;
    if (!WifiEnsureForScan() || result.cancelled.load()) {
        return;
    }
    WifiDiagnosticScan scan;
    scan.records.resize(64); // 事件回调只缩小，不在系统事件任务分配内存
    scan.events = xEventGroupCreate();
    if (!scan.events) {
        ESP_LOGE(kTag, "operation=scan stage=event_group result=no_memory");
        return;
    }
    esp_event_handler_instance_t handler = nullptr;
    auto error = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, OnScanDone, &scan, &handler);
    if (error == ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_events = scan.events;
        }
        wifi_scan_config_t config{};
        config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        esp_wifi_scan_stop();
        {
            std::lock_guard<std::mutex> lock(scan.mutex);
            scan.captured = false;
            scan.success = false;
            scan.records.resize(64);
            xEventGroupClearBits(scan.events, 1);
        }
        if (!result.cancelled.load()) {
            error = esp_wifi_scan_start(&config, false);
            if (error == ESP_OK) {
                xEventGroupWaitBits(scan.events, 1, pdTRUE, pdTRUE, pdMS_TO_TICKS(15000));
            }
        }
        esp_wifi_scan_stop();
        bool reported = false;
        for (;;) {
            const auto unregistered = esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, handler);
            if (unregistered == ESP_OK) {
                break;
            }
            if (!reported) {
                ESP_LOGE(kTag, "operation=scan stage=unregister error=%d; retaining_session", int(unregistered));
                reported = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_events = nullptr;
    }
    vEventGroupDelete(scan.events);
    esp_wifi_clear_ap_list();
    if (result.cancelled.load()) {
        return;
    }
    if (error != ESP_OK || !scan.captured || !scan.success) {
        ESP_LOGW(kTag, "operation=scan stage=complete error=%d captured=%d", int(error), int(scan.captured));
        return;
    }
    std::sort(scan.records.begin(), scan.records.end(), [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    for (const auto& record : scan.records) {
        char ssid[33];
        std::memcpy(ssid, record.ssid, 32);
        ssid[32] = '\0';
        if (ssid[0] && std::none_of(result.access_points.begin(), result.access_points.end(),
                                   [ssid](const auto& item) { return item.ssid == ssid; })) {
            result.access_points.push_back({ssid, record.rssi});
        }
    }
    result.success = true;
}

static void ScanWorker(void* context) {
    WifiDiagnosticRequest result_owner;
    {
        std::unique_ptr<WifiDiagnosticRequest> owner(static_cast<WifiDiagnosticRequest*>(context));
        auto& result = **owner;
        result_owner = *owner;
        PowerNeedHold hold(PowerNeed::StandbyInhibit);
        s_worker_generation = result.generation;
        const bool radio = WifiRadio::GetInstance().BeginStationSetup();
        if (radio) {
            try {
                RunScan(result);
            } catch (const std::bad_alloc&) {
                ESP_LOGE(kTag, "operation=scan stage=execute result=no_memory");
            }
            // 只释放本检测创建的驱动，已连接的Station始终归原所有者。
            for (;;) {
                WifiLifecycleLock lock;
                if (WifiTeardownLocked()) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            WifiRadio::GetInstance().EndStationSetup();
        } else {
            ESP_LOGW(kTag, "operation=scan stage=radio result=busy");
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_running = false;
        }
    }
    result_owner->completed.store(true, std::memory_order_release);
    result_owner.reset();
    vTaskDelete(nullptr);
}

WifiDiagnosticRequest WifiDiagnosticStart() try {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_running) {
        return {};
    }
    auto result = std::make_shared<WifiDiagnosticResult>();
    auto owner = std::make_unique<WifiDiagnosticRequest>(result);
    result->generation = s_scan_generation.fetch_add(1) + 1;
    s_running = true;
    if (FirmwareCreateTask(ScanWorker, "wifi_diagnostic", kStackBytes, owner.get(), 5, nullptr, tskNO_AFFINITY, 0) != pdPASS) {
        s_running = false;
        ESP_LOGW(kTag, "operation=scan stage=create_task result=unavailable");
        return {};
    }
    owner.release();
    s_request = result;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(kTag, "operation=scan stage=submit result=no_memory");
    return {};
}

bool WifiDiagnosticIsRunning() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_running;
}

void WifiDiagnosticCancel(const WifiDiagnosticRequest& request) {
    if (!request) {
        return;
    }
    request->cancelled.store(true);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_request.lock() == request) {
        s_scan_generation.fetch_add(1);
        if (s_events) {
            xEventGroupSetBits(s_events, 1);
        }
    }
}
