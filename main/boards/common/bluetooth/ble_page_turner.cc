#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "infra/tasks/task_create.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ble_page_turner.h"
#include "free2_buttons.h"
#include "free3r_buttons.h"
#include "hbtr_gesture_buttons.h"
#include "smart1e_buttons.h"

static constexpr const char* TAG = "BlePageTurner";

static constexpr bool IsHbtrGestureModel(BlePageTurnerModel model) {
    return model == BlePageTurnerModel::kHbtr002X || model == BlePageTurnerModel::kHbtr003X;
}

static const ble_uuid16_t kService = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t kReport = BLE_UUID16_INIT(0x2a4d);
static std::mutex s_mutex;
static BlePageTurnerSnapshot s_status;
static ble_addr_t s_addresses[kBlePageTurnerMaxDevices];

struct BlePageTurnerProfile {
    ble_addr_t address{};             // 保存的 BLE 身份地址
    BlePageTurnerSavedDevice device;  // 型号及翻页按键绑定
};

static std::vector<BlePageTurnerProfile> s_profiles;
static uint32_t s_saved_revision = UINT32_MAX;
static bool s_initialized = false;
static bool s_boot_pending = false;
static int64_t s_boot_not_before = 0;
static bool s_profiles_loaded = false;  // 由 s_mutex 保护，配置只加载一次
static std::atomic<BlePageTurnerCommandHandler> s_handler{nullptr};
static std::atomic<bool> s_started{false};
static std::atomic<bool> s_synced{false};
static std::atomic<bool> s_host_running{false};  // 主机任务已建立，允许在未同步或重置后停止
static ble_npl_event s_control;
static uint8_t s_pressed_key = 0;  // 过滤同一协议按压中的重复非零报告
static HbtrGestureButtonsDecoder s_hbtr_gesture_buttons;
enum class BlePageTurnerRequest {
    kBind,          // 保存学习得到的按键绑定
    kAutoScan,      // 自动连接扫描
    kAutoConnect,   // 自动寻找过程中连接候选设备
    kEnable,        // 开启自动连接
    kSuspend,       // 待机暂停，保留开关
    kPersist,       // 保存开关
    kNone,          // 无待处理请求
    kScan,          // 扫描设备
    kRefreshScan,   // 保留结果的后台扫描
    kConnect,       // 连接所选设备
    kConnectSaved,  // 连接保存的身份地址
    kForget,        // 清除保存记录及配对密钥
};
static BlePageTurnerRequest s_request = BlePageTurnerRequest::kNone;
static int s_index = -1;
static ble_addr_t s_requested_address{};
static BlePageTurnerModel s_requested_model = BlePageTurnerModel::kFree2;
static uint8_t s_binding_key = 0;
static bool s_switching = false;
static ble_addr_t s_switch_address{};
static BlePageTurnerModel s_switch_model = BlePageTurnerModel::kFree2;
static uint8_t s_own_address = 0;
static BlePageTurnerModel s_model = BlePageTurnerModel::kFree2;
static uint16_t s_connection = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_service_start = 0, s_service_end = 0;

struct BlePageTurnerReport {
    uint16_t value = 0;      // 通知值句柄
    uint16_t end = 0;        // 特征描述符范围末尾
    uint16_t cccd = 0;       // 客户端通知配置句柄
    uint16_t reference = 0;  // Report Reference 描述符句柄
    uint8_t id = 0;          // HID 报告编号，用于订阅日志
    bool buttons = false;    // 已验证按键协议的输入报告
};

static BlePageTurnerReport s_reports[8];
static int s_report_count = 0, s_subscribing = 0;
static int s_input_reports = 0;
static int s_active_button_report = -1;
static BlePageTurnerState s_after_disconnect = BlePageTurnerState::kIdle;
static bool s_cancelled = false;
static bool s_pairing = false;
static bool s_pairing_waiting = false;
static bool s_pairing_reference = false;
static std::atomic<bool> s_stopping{false};
static std::atomic<bool> s_host_stopped{false};
static std::atomic<bool> s_suspended{false};
static bool s_release_pending = false;  // 由 s_mutex 保护，完整释放前禁止重新启动
static bool s_release_queued = false;   // 由 s_mutex 保护，复用现有后台回收任务
static bool s_scan_pending = false;     // 释放期间重新进入设备管理，完成后再扫描
static int64_t s_release_retry = 0;     // 初始化未完成或释放失败时延后重试
static bool s_auto_scan = false;
static bool s_manual_discovery = false;     // 由 s_mutex 保护，设备管理持有扫描
static bool s_stop_scan_requested = false;  // 由 BLE 任务取消扫描，不覆盖连接等请求
static bool s_scan_seen[kBlePageTurnerMaxDevices] = {};
static bool s_enabled_dirty = false;
static std::mutex s_persist_mutex;
static std::atomic<bool> s_preference_queued{false};
static int64_t s_auto_deadline = 0;
static int64_t s_next_auto_scan = 0;
static int64_t s_persist_retry = 0;
static int64_t s_init_retry = 0;
static ble_addr_t s_auto_tried[kBlePageTurnerMaxDevices];
static int s_auto_tried_count = 0;
static constexpr int64_t kAutoTimeoutUs = 30000000;
extern "C" void ble_store_config_init();

static int GapEvent(ble_gap_event* event, void*);
static void Fail(int error);
static void StopActivities();
static bool Submit(BlePageTurnerRequest request, int index = -1, bool replace = true, const char* address = nullptr);
static void QueueRelease();
static void StartScan(bool automatic = false, bool keep_results = false);

static void BeginAutoLocked() {
    s_status.searching = true;
    s_status.timed_out = false;
    s_auto_deadline = esp_timer_get_time() + kAutoTimeoutUs;
    s_next_auto_scan = 0;
    s_auto_tried_count = 0;
}

static void EnableLocked(bool enabled) {
    if (s_status.enabled != enabled) {
        s_enabled_dirty = true;
    }
    s_status.enabled = enabled;
    if (!enabled) {
        s_auto_deadline = 0;
        s_status.searching = false;
    }
    ++s_status.revision;
}

static void PersistEnabled() {
    std::lock_guard<std::mutex> persistence_lock(s_persist_mutex);
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_enabled_dirty) {
            return;
        }
        enabled = s_status.enabled;
    }
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("pager", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_u8(handle, "enabled", enabled);
        if (rc == ESP_OK) {
            rc = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (rc == ESP_OK && s_status.enabled == enabled) {
        s_enabled_dirty = false;
    } else if (rc != ESP_OK) {
        ESP_LOGW(TAG, "Save enabled failed: %s", esp_err_to_name(rc));
    }
    s_persist_retry = esp_timer_get_time() + 1000000;
}

static void QueuePreferenceWrite() {
    if (s_preference_queued.exchange(true)) {
        return;
    }
    if (!TaskExecutor::Schedule(
            []() {
                PersistEnabled();
                s_preference_queued = false;
            },
            TaskDomain::Storage)) {
        s_preference_queued = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_persist_retry = esp_timer_get_time() + 1000000;
    }
}

static int Descriptor(uint16_t connection, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* descriptor,
                      void*);

static void CancelInput() {
    s_hbtr_gesture_buttons.Reset();
    if (s_pressed_key) {
        ESP_LOGD(TAG, "button=%c phase=cancel", 'A' + s_pressed_key - 1);
    }
    s_pressed_key = 0;
    if (auto handler = s_handler.load()) {
        handler(BlePageTurnerAction::kPrevious, BlePageTurnerKeyPhase::kCancel);
    }
}

// 调用方持有 s_mutex，保证解码、取消及输入分发不会交错。
static void InputLocked(uint8_t key) {
    if (s_release_pending) {
        return;
    }
    if (key == s_pressed_key) {
        return;
    }
    s_pressed_key = key;
    if (!key) {
        return;
    }
    if (s_status.learning) {
        if (s_status.binding_pending || key == kFree3RButtonsStandbyKey) {
            return;
        }
        s_status.learned_key = key;
        ++s_status.revision;
        return;
    }
    if (s_status.active_saved < 0) {
        return;
    }
    if ((s_model == BlePageTurnerModel::kFree3R || s_model == BlePageTurnerModel::kSmart1E ||
         IsHbtrGestureModel(s_model)) &&
        key == kFree3RButtonsStandbyKey) {
        ESP_LOGD(TAG, "button=C action=standby phase=click");
        if (auto handler = s_handler.load()) {
            handler(BlePageTurnerAction::kStandby, BlePageTurnerKeyPhase::kClick);
        }
        return;
    }
    const auto& bindings = s_profiles[s_status.active_saved].device.keys;
    const int action = key == bindings[0] ? 0 : key == bindings[1] ? 1 : -1;
    if (action < 0) {
        return;
    }
    ESP_LOGD(TAG, "button=%c action=%s phase=click", 'A' + key - 1, action == 0 ? "previous" : "next");
    if (auto handler = s_handler.load()) {
        handler(static_cast<BlePageTurnerAction>(action), BlePageTurnerKeyPhase::kClick);
    }
}

static void FormatAddress(char* text, const ble_addr_t& address) {
    const auto* a = address.val;
    std::snprintf(text, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// 固定字节布局避免持久化结构体填充；写入成功后再发布配置变化。
static bool SaveProfiles() {
    std::vector<uint8_t> bytes(1 + s_profiles.size() * 10);
    bytes[0] = 5;
    for (size_t i = 0; i < s_profiles.size(); ++i) {
        auto* record = bytes.data() + 1 + i * 10;
        const auto& profile = s_profiles[i];
        record[0] = profile.address.type;
        std::memcpy(record + 1, profile.address.val, 6);
        std::memcpy(record + 7, profile.device.keys, 2);
        record[9] = static_cast<uint8_t>(profile.device.model);
    }
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("pager", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_blob(handle, "devices", bytes.data(), bytes.size());
        if (rc == ESP_OK) {
            rc = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "Save devices failed: %s", esp_err_to_name(rc));
    }
    return rc == ESP_OK;
}

static void LoadProfiles() {
    nvs_handle_t handle;
    if (nvs_open("pager", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t length = 0;
    esp_err_t rc = nvs_get_blob(handle, "devices", nullptr, &length);
    if (rc != ESP_OK || !length) {
        nvs_close(handle);
        return;
    }
    std::vector<uint8_t> bytes(length);
    rc = nvs_get_blob(handle, "devices", bytes.data(), &length);
    nvs_close(handle);
    if (rc != ESP_OK || !length || bytes[0] < 1 || bytes[0] > 5) {
        return;
    }
    const bool legacy = bytes[0] < 5;
    const size_t stride = bytes[0] == 3 ? 9 : 10;
    const size_t offset = legacy ? 2 : 1;
    if (length < offset || (length - offset) % stride) {
        return;
    }
    size_t count = (length - offset) / stride;
    if (legacy) {
        constexpr size_t kLegacySlots = 3;  // 旧格式固定槽位，仅用于迁移
        if (count != kLegacySlots || bytes[1] > kLegacySlots) {
            return;
        }
        count = bytes[1];
    }
    for (size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + offset + i * stride;
        if (record[0] > BLE_ADDR_RANDOM_ID || (record[7] && record[7] == record[8]) ||
            (bytes[0] >= 4 && record[9] >= static_cast<uint8_t>(BlePageTurnerModel::kCount)) ||
            (!legacy && (record[7] > 2 || record[8] > 2))) {
            return;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto* previous = bytes.data() + offset + j * stride;
            if (record[0] == previous[0] && !std::memcmp(record + 1, previous + 1, 6)) {
                return;
            }
        }
    }
    s_profiles.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + offset + i * stride;
        auto& profile = s_profiles[i];
        profile.address.type = record[0];
        std::memcpy(profile.address.val, record + 1, 6);
        FormatAddress(profile.device.address, profile.address);
        profile.device.model = bytes[0] >= 4 ? static_cast<BlePageTurnerModel>(record[9]) : BlePageTurnerModel::kFree2;
        for (int action = 0; action < 2; ++action) {
            profile.device.keys[action] = record[7 + action] <= 2 ? record[7 + action] : 0;
        }
    }
    if (legacy) {
        SaveProfiles();
    }
}

static int FindSaved(const ble_addr_t& address) {
    for (size_t i = 0; i < s_profiles.size(); ++i) {
        if (!ble_addr_cmp(&s_profiles[i].address, &address)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool RememberConnected() {
    ble_gap_conn_desc description{};
    if (ble_gap_conn_find(s_connection, &description)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_status.enabled || s_suspended) {
        return false;
    }
    int index = FindSaved(description.peer_id_addr);
    const bool added = index < 0;
    if (added) {
        index = static_cast<int>(s_profiles.size());
        s_profiles.push_back({description.peer_id_addr, {}});
        FormatAddress(s_profiles[index].device.address, description.peer_id_addr);
    }
    const auto previous_model = s_profiles[index].device.model;
    s_profiles[index].device.model = s_model;
    if ((added || previous_model != s_model) && !SaveProfiles()) {
        if (added) {
            s_profiles.pop_back();
        } else {
            s_profiles[index].device.model = previous_model;
        }
        return false;
    }
    s_status.active_saved = index;
    s_auto_deadline = 0;
    s_status.searching = false;
    ++s_status.revision;
    return true;
}

static void DiscoverDescriptor() {
    const auto& report = s_reports[s_subscribing];
    const int rc = ble_gattc_disc_all_dscs(s_connection, report.value, report.end, Descriptor, nullptr);
    if (rc) {
        Fail(rc);
    }
}

static void SetStateLocked(BlePageTurnerState state) {
    if (state != BlePageTurnerState::kConnected) {
        CancelInput();
    }
    s_status.state = state;
    if (state != BlePageTurnerState::kConnected) {
        s_status.learning = false;
        s_status.learned_key = 0;
        s_status.active_saved = -1;
        if (s_status.binding_pending) {
            s_status.binding_pending = false;
            s_status.operation_failed = true;
        }
    }
    ++s_status.revision;
}

static void SetState(BlePageTurnerState state) {
    std::lock_guard<std::mutex> lock(s_mutex);
    SetStateLocked(state);
}

static void Fail(int error) {
    ESP_LOGW(TAG, "BLE operation failed: %d", error);
    s_after_disconnect = BlePageTurnerState::kError;
    SetState(BlePageTurnerState::kError);
    if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int Subscribed(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr*, void*);
static int Referenced(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attr, void*);

static void ReadReference() {
    const int rc = ble_gattc_read(s_connection, s_reports[s_subscribing].reference, Referenced, nullptr);
    if (rc) {
        Fail(rc);
    }
}

static void EnableNotifications() {
    const uint8_t notify[] = {1, 0};
    const int rc =
        ble_gattc_write_flat(s_connection, s_reports[s_subscribing].cccd, notify, sizeof(notify), Subscribed, nullptr);
    if (rc) {
        Fail(rc);
    }
}

static bool RetryWithEncryption(uint16_t connection, int status, bool reference) {
    if (s_pairing || (status != BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) &&
                      status != BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC))) {
        return false;
    }
    s_pairing = true;
    s_pairing_waiting = true;
    s_pairing_reference = reference;
    const int rc = ble_gap_security_initiate(connection);
    if (rc) {
        Fail(rc);
    }
    return true;
}

static void NextReport() {
    if (++s_subscribing < s_report_count) {
        DiscoverDescriptor();
    } else if (!s_input_reports) {
        Fail(BLE_HS_ENOTSUP);
    } else if (!RememberConnected()) {
        if (!BlePageTurnerGetSnapshot().enabled || s_suspended) {
            StopActivities();
        } else {
            Fail(BLE_HS_ESTORE_CAP);
        }
    } else {
        SetState(BlePageTurnerState::kConnected);
        PersistEnabled();
        ESP_LOGI(TAG, "%s input reports ready, reports=%d", BlePageTurnerModelName(s_model), s_input_reports);
    }
}

static int Subscribed(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    if (connection != s_connection) {
        return 0;
    }
    if (RetryWithEncryption(connection, error->status, false)) {
        return 0;
    }
    if (error->status) {
        Fail(error->status);
    } else {
        ESP_LOGI(TAG, "Subscribed: report=%u handle=%u cccd=%u", s_reports[s_subscribing].id,
                 s_reports[s_subscribing].value, s_reports[s_subscribing].cccd);
        NextReport();
    }
    return 0;
}

static int Referenced(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
    if (connection != s_connection) {
        return 0;
    }
    if (RetryWithEncryption(connection, error->status, true)) {
        return 0;
    }
    if (error->status) {
        Fail(error->status);
        return 0;
    }
    uint8_t reference[2];
    if (!attr || !attr->om || OS_MBUF_PKTLEN(attr->om) != sizeof(reference) ||
        os_mbuf_copydata(attr->om, 0, sizeof(reference), reference)) {
        Fail(BLE_HS_EBADDATA);
    } else {
        ESP_LOGI(TAG, "Report reference: model=%s handle=%u id=%u type=%u cccd=%u", BlePageTurnerModelName(s_model),
                 s_reports[s_subscribing].value, reference[0], reference[1], s_reports[s_subscribing].cccd);
        if (reference[1] == kFree2ButtonsInputReport) {
            s_reports[s_subscribing].id = reference[0];
            if (!s_reports[s_subscribing].cccd) {
                Fail(BLE_HS_EBADDATA);
            } else {
                s_reports[s_subscribing].buttons =
                    (s_model == BlePageTurnerModel::kFree2 && Free2ButtonsIsButtonReport(reference[0], reference[1])) ||
                    (s_model == BlePageTurnerModel::kFree3R &&
                     Free3RButtonsIsButtonReport(reference[0], reference[1])) ||
                    (s_model == BlePageTurnerModel::kSmart1E &&
                     Smart1EButtonsIsButtonReport(reference[0], reference[1])) ||
                    (IsHbtrGestureModel(s_model) && HbtrGestureButtonsIsButtonReport(reference[0], reference[1]));
                ++s_input_reports;
                EnableNotifications();
            }
        } else {
            NextReport();
        }
    }
    return 0;
}

static int Descriptor(uint16_t connection, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* descriptor,
                      void*) {
    if (connection != s_connection) {
        return 0;
    }
    auto& report = s_reports[s_subscribing];
    if (!error->status) {
        const uint16_t uuid = ble_uuid_u16(&descriptor->uuid.u);
        if (uuid == 0x2902) {
            report.cccd = descriptor->handle;
        } else if (uuid == 0x2908) {
            report.reference = descriptor->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (report.reference) {
            ReadReference();
        } else {
            NextReport();
        }
    } else {
        Fail(error->status);
    }
    return 0;
}

static int Characteristic(uint16_t connection, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
    if (connection != s_connection) {
        return 0;
    }
    if (!error->status) {
        // 以相邻特征声明为边界，避免订阅到下一个 HID Report 的 CCCD。
        if (s_report_count && !s_reports[s_report_count - 1].end) {
            s_reports[s_report_count - 1].end = chr->def_handle - 1;
        }
        if (ble_uuid_cmp(&chr->uuid.u, &kReport.u) == 0 && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            if (s_report_count == static_cast<int>(sizeof(s_reports) / sizeof(s_reports[0]))) {
                Fail(BLE_HS_ENOMEM);
                return BLE_HS_ENOMEM;
            }
            s_reports[s_report_count++] = {chr->val_handle, 0, 0};
        }
    } else if (error->status == BLE_HS_EDONE && s_report_count) {
        if (!s_reports[s_report_count - 1].end) {
            s_reports[s_report_count - 1].end = s_service_end;
        }
        s_subscribing = 0;
        DiscoverDescriptor();
    } else {
        Fail(error->status);
    }
    return 0;
}

static int Service(uint16_t connection, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (connection != s_connection) {
        return 0;
    }
    if (!error->status && !s_service_start) {
        s_service_start = service->start_handle;
        s_service_end = service->end_handle;
    } else if (error->status == BLE_HS_EDONE && s_service_start) {
        const int rc = ble_gattc_disc_all_chrs(connection, s_service_start, s_service_end, Characteristic, nullptr);
        if (rc) {
            Fail(rc);
        }
    } else if (error->status) {
        Fail(error->status);
    }
    return 0;
}

static void StartScan(bool automatic, bool keep_results) {
    if (ble_gap_conn_active() || s_suspended) {
        return;
    }
    if (ble_gap_disc_active()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!keep_results) {
            s_status.count = 0;
        }
        std::memset(s_scan_seen, 0, sizeof(s_scan_seen));
        s_status.scanning = true;
        if (s_status.state == BlePageTurnerState::kError) {
            s_status.state = BlePageTurnerState::kIdle;
        }
        ++s_status.revision;
    }
    s_auto_scan = automatic;
    ble_gap_disc_params params{};
    params.passive = 0;
    params.itvl = 0x80;
    params.window = 0x30;
    const int duration = automatic ? 3000 : 5000;
    ESP_LOGD(TAG, "Scanning page turners, duration=%dms auto=%d", duration, automatic);
    const int rc = ble_gap_disc(s_own_address, duration, &params, GapEvent, nullptr);
    if (rc) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status.scanning = false;
        }
        Fail(rc);
    }
}

static void TryAutomaticConnection() {
    int candidate = -1;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_status.enabled || !s_status.searching || s_suspended || s_auto_deadline <= esp_timer_get_time()) {
            return;
        }
        bool candidate_saved = false;
        for (int i = 0; i < s_status.count; ++i) {
            bool tried = false;
            for (int j = 0; j < s_auto_tried_count; ++j) {
                if (!ble_addr_cmp(&s_auto_tried[j], &s_addresses[i])) {
                    tried = true;
                }
            }
            if (tried) {
                continue;
            }
            const bool saved = FindSaved(s_addresses[i]) >= 0;
            if (candidate < 0 || (saved && !candidate_saved) ||
                (saved == candidate_saved && s_status.devices[i].rssi > s_status.devices[candidate].rssi)) {
                candidate = i;
                candidate_saved = saved;
            }
        }
        s_next_auto_scan = esp_timer_get_time() + 1000000;
        if (candidate >= 0 && s_auto_tried_count < kBlePageTurnerMaxDevices) {
            s_auto_tried[s_auto_tried_count++] = s_addresses[candidate];
        } else if (candidate < 0) {
            s_auto_tried_count = 0;
        }
    }
    if (candidate >= 0) {
        Submit(BlePageTurnerRequest::kAutoConnect, candidate, false);
    }
}

static void StopActivities() {
    s_switching = false;
    s_cancelled = true;
    s_after_disconnect = BlePageTurnerState::kIdle;
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    if (ble_gap_conn_active()) {
        ble_gap_conn_cancel();
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.scanning = false;
    }
    if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
        if (rc && rc != BLE_HS_EALREADY) {
            Fail(rc);
        }
    } else {
        SetState(BlePageTurnerState::kIdle);
    }
}

static void ConnectTo(const ble_addr_t& address, BlePageTurnerModel model) {
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    s_after_disconnect = BlePageTurnerState::kIdle;
    s_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || s_suspended || s_request == BlePageTurnerRequest::kSuspend) {
            return;
        }
        s_status.scanning = false;
        EnableLocked(true);
        FormatAddress(s_status.target_address, address);
        s_model = model;
        s_status.target_model = model;
        s_status.state = BlePageTurnerState::kConnecting;
        ++s_status.revision;
    }
    PersistEnabled();
    const int rc = ble_gap_connect(s_own_address, &address, 10000, nullptr, GapEvent, nullptr);
    if (rc) {
        Fail(rc);
    }
}

static void Control(ble_npl_event*) {
    BlePageTurnerRequest request;
    int index;
    ble_addr_t address{};
    BlePageTurnerModel model;
    bool stop_scan;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        request = s_request;
        index = s_index;
        s_request = BlePageTurnerRequest::kNone;
        address = s_requested_address;
        model = s_requested_model;
        stop_scan = s_stop_scan_requested;
        s_stop_scan_requested = false;
        if (request == BlePageTurnerRequest::kConnectSaved || request == BlePageTurnerRequest::kForget) {
            index = FindSaved(address);
        }
    }
    if (stop_scan) {
        if (ble_gap_disc_active()) {
            const int rc = ble_gap_disc_cancel();
            if (rc && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Cancel scan failed: %d", rc);
            }
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.scanning = false;
        ++s_status.revision;
        ESP_LOGI(TAG, "Device manager scan stopped");
    }
    if ((request == BlePageTurnerRequest::kConnectSaved || request == BlePageTurnerRequest::kForget) && index < 0) {
        return;
    }
    PersistEnabled();
    if (request == BlePageTurnerRequest::kBind) {
        std::lock_guard<std::mutex> lock(s_mutex);
        const int saved = FindSaved(address);
        bool success = saved >= 0 && s_status.state == BlePageTurnerState::kConnected && s_status.active_saved == saved;
        if (success) {
            auto& device = s_profiles[saved].device;
            const auto before = device;
            const uint8_t key = s_binding_key;
            if (key && device.keys[1 - index] == key) {
                device.keys[1 - index] = 0;
            }
            device.keys[index] = key;
            success = SaveProfiles();
            if (!success) {
                device = before;
            } else {
                ESP_LOGI(TAG, "Binding saved: action=%s key=%u", index == 0 ? "previous" : "next", key);
                CancelInput();
                s_status.learning = false;
                s_status.learned_key = 0;
            }
        }
        s_status.binding_pending = false;
        s_status.operation_failed = !success;
        ++s_status.revision;
    } else if (request == BlePageTurnerRequest::kSuspend) {
        StopActivities();
    } else if (request == BlePageTurnerRequest::kEnable || request == BlePageTurnerRequest::kAutoScan) {
        if (BlePageTurnerGetSnapshot().enabled && !s_suspended && s_connection == BLE_HS_CONN_HANDLE_NONE) {
            StartScan(true);
        }
    } else if (request == BlePageTurnerRequest::kScan || request == BlePageTurnerRequest::kRefreshScan) {
        if (ble_gap_disc_active() && (s_auto_scan || request == BlePageTurnerRequest::kScan)) {
            const int rc = ble_gap_disc_cancel();
            if (rc && rc != BLE_HS_EALREADY) {
                Fail(rc);
                return;
            }
        }
        StartScan(false, request == BlePageTurnerRequest::kRefreshScan);
    } else if (request == BlePageTurnerRequest::kConnect || request == BlePageTurnerRequest::kConnectSaved ||
               request == BlePageTurnerRequest::kAutoConnect) {
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
        }
        if (ble_gap_conn_active()) {
            return;
        }
        if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_conn_desc connected{};
            if (!ble_gap_conn_find(s_connection, &connected) && !ble_addr_cmp(&connected.peer_id_addr, &address)) {
                return;
            }
            s_switch_address = address;
            s_switch_model = model;
            s_switching = true;
            s_after_disconnect = BlePageTurnerState::kIdle;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                EnableLocked(true);
                FormatAddress(s_status.target_address, address);
                s_status.target_model = model;
            }
            SetState(BlePageTurnerState::kConnecting);
            const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
            if (rc) {
                s_switching = false;
                Fail(rc);
            }
        } else {
            ConnectTo(address, model);
        }
    } else if (request == BlePageTurnerRequest::kForget) {
        if (ble_gap_conn_active()) {
            return;
        }
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
        }
        s_after_disconnect = BlePageTurnerState::kIdle;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_status.active_saved == index) {
                EnableLocked(false);
                s_switching = false;
            }
            s_status.scanning = false;
        }
        PersistEnabled();
        const int rc = ble_gap_unpair(&address);
        if (rc && rc != BLE_HS_ENOENT) {
            Fail(rc);
            return;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        CancelInput();
        const auto removed = s_profiles[index];
        s_profiles.erase(s_profiles.begin() + index);
        if (!SaveProfiles()) {
            s_profiles.insert(s_profiles.begin() + index, removed);
            s_status.state = BlePageTurnerState::kError;
        } else {
            if (s_status.active_saved == index) {
                s_status.active_saved = -1;
            } else if (s_status.active_saved > index) {
                --s_status.active_saved;
            }
        }
        ++s_status.revision;
    }
}

static int GapEvent(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            ble_hs_adv_fields fields{};
            const auto& found = event->disc;
            if (ble_hs_adv_parse_fields(&fields, found.data, found.length_data) || !fields.name) {
                break;
            }
            int model = 0;
            for (; model < static_cast<int>(BlePageTurnerModel::kCount); ++model) {
                const char* name = BlePageTurnerModelName(static_cast<BlePageTurnerModel>(model));
                if (fields.name_len == std::strlen(name) && !std::memcmp(fields.name, name, fields.name_len)) {
                    break;
                }
            }
            if (model == static_cast<int>(BlePageTurnerModel::kCount)) {
                break;
            }
            std::lock_guard<std::mutex> lock(s_mutex);
            int index = 0;
            while (index < s_status.count && ble_addr_cmp(&s_addresses[index], &found.addr)) {
                ++index;
            }
            if (index == kBlePageTurnerMaxDevices) {
                break;
            }
            s_scan_seen[index] = true;
            if (index == s_status.count) {
                s_addresses[index] = found.addr;
                s_status.devices[index].model = static_cast<BlePageTurnerModel>(model);
                FormatAddress(s_status.devices[index].address, found.addr);
                ESP_LOGI(TAG, "Found %s address=%s", BlePageTurnerModelName(s_status.devices[index].model),
                         s_status.devices[index].address);
                ++s_status.count;
                ++s_status.revision;
            }
            if (s_status.devices[index].rssi != found.rssi) {
                s_status.devices[index].rssi = found.rssi;
                ++s_status.revision;
            }
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE: {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_auto_scan) {
                int kept = 0;
                for (int i = 0; i < s_status.count; ++i) {
                    if (s_scan_seen[i]) {
                        s_addresses[kept] = s_addresses[i];
                        s_status.devices[kept++] = s_status.devices[i];
                    }
                }
                s_status.count = kept;
            }
            s_status.scanning = false;
            ++s_status.revision;
            ESP_LOGD(TAG, "Scan complete: count=%d auto=%d reason=%d", s_status.count, s_auto_scan,
                     event->disc_complete.reason);
        }
            if (s_auto_scan && s_connection == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) {
                TryAutomaticConnection();
            }
            break;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status) {
                if (!s_cancelled) {
                    ESP_LOGW(TAG, "Connect failed: %d", event->connect.status);
                }
                SetState(s_cancelled ? BlePageTurnerState::kIdle : BlePageTurnerState::kError);
                break;
            }
            s_connection = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected, handle=%u; discovering HID reports", s_connection);
            if (s_cancelled) {
                const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
                if (rc) {
                    Fail(rc);
                }
                break;
            }
            s_pairing = false;
            s_pairing_waiting = false;
            s_input_reports = 0;
            s_active_button_report = -1;
            s_service_start = s_service_end = 0;
            s_report_count = s_subscribing = 0;
            {
                const int rc = ble_gattc_disc_svc_by_uuid(s_connection, &kService.u, Service, nullptr);
                if (rc) {
                    Fail(rc);
                }
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected, handle=%u reason=%d", event->disconnect.conn.conn_handle,
                     event->disconnect.reason);
            s_connection = BLE_HS_CONN_HANDLE_NONE;
            s_report_count = 0;
            SetState(s_after_disconnect);
            if (s_switching) {
                s_switching = false;
                if (BlePageTurnerGetSnapshot().enabled && !s_suspended) {
                    ConnectTo(s_switch_address, s_switch_model);
                }
            } else {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (s_after_disconnect != BlePageTurnerState::kError && s_status.enabled && !s_suspended &&
                    !s_stopping && !s_status.searching) {
                    BeginAutoLocked();
                }
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.conn_handle == s_connection && s_pairing_waiting) {
                s_pairing_waiting = false;
                if (event->enc_change.status) {
                    Fail(event->enc_change.status);
                } else if (s_pairing_reference) {
                    ReadReference();
                } else {
                    EnableNotifications();
                }
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            const auto& report = event->notify_rx;
            const int length = OS_MBUF_PKTLEN(report.om);
            if (ESP_LOG_ENABLED(ESP_LOG_DEBUG) && esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
                int report_id = -1;
                for (int i = 0; i < s_report_count; ++i) {
                    if (s_reports[i].value == report.attr_handle) {
                        report_id = s_reports[i].id;
                        break;
                    }
                }
                if (!length) {
                    ESP_LOGD(TAG, "Notify conn=%u handle=%u report=%d len=0", report.conn_handle, report.attr_handle,
                             report_id);
                }
                for (int offset = 0; offset < length; offset += 16) {
                    uint8_t raw[16];
                    const int copied = length - offset < 16 ? length - offset : 16;
                    char text[sizeof(raw) * 3 + 1] = {};
                    if (os_mbuf_copydata(report.om, offset, copied, raw)) {
                        ESP_LOGW(TAG, "Notify read failed, handle=%u len=%d offset=%d", report.attr_handle, length,
                                 offset);
                        break;
                    }
                    for (int i = 0; i < copied; ++i) {
                        std::snprintf(text + 3 * i, 4, "%02x ", raw[i]);
                    }
                    ESP_LOGD(TAG, "Notify conn=%u handle=%u report=%d len=%d offset=%d data=%s", report.conn_handle,
                             report.attr_handle, report_id, length, offset, text);
                }
            }
            if (report.conn_handle != s_connection || !s_input_reports ||
                BlePageTurnerGetSnapshot().state != BlePageTurnerState::kConnected || length < 1 || length > 4) {
                break;
            }
            int index = 0;
            while (index < s_report_count &&
                   (s_reports[index].value != report.attr_handle || !s_reports[index].buttons)) {
                ++index;
            }
            if (index == s_report_count) {
                break;
            }
            uint8_t bytes[4];
            if (os_mbuf_copydata(report.om, 0, length, bytes)) {
                ESP_LOGW(TAG, "Input read failed: handle=%u len=%d", report.attr_handle, length);
                break;
            }
            std::lock_guard<std::mutex> lock(s_mutex);
            if (IsHbtrGestureModel(s_model)) {
                const auto key = s_hbtr_gesture_buttons.Decode(s_reports[index].id, bytes, length);
                if (key != kHbtrGestureButtonsInvalid) {
                    InputLocked(key);
                    InputLocked(0);
                }
                break;
            }
            const uint8_t key = s_model == BlePageTurnerModel::kSmart1E ? Smart1EButtonsDecode(bytes, length)
                                : s_model == BlePageTurnerModel::kFree3R
                                    ? Free3RButtonsDecode(bytes, length)
                                    : Free2ButtonsDecode(s_reports[index].id, bytes, length);
            if (key == kFree2ButtonsInvalid) {
                break;
            }
            if (s_active_button_report < 0 && key) {
                s_active_button_report = index;
                ESP_LOGI(TAG, "Button input selected: report=%u handle=%u", s_reports[index].id, report.attr_handle);
            }
            // 同一连接固定使用首个有效按键通道，避免其他报告的零值干扰去重。
            if (index == s_active_button_report) {
                InputLocked(key);
                // 单字节方向指令没有松开通知，立即结束内部按压状态以保留连续同向短按。
                if ((s_model == BlePageTurnerModel::kFree2 && s_reports[index].id == kFree2ButtonsClickReportId) ||
                    (s_model == BlePageTurnerModel::kSmart1E && s_reports[index].id == kSmart1EButtonsClickReportId)) {
                    InputLocked(0);
                }
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

static void Synced() {
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) {
        rc = ble_hs_id_infer_auto(0, &s_own_address);
    }
    if (rc) {
        Fail(rc);
        return;
    }
    s_synced = true;
    // 导入本模块此前已建立的配对，使升级前的设备也可在管理页重连或忘记。
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (!ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            if (FindSaved(peers[i]) >= 0) {
                continue;
            }
            s_profiles.push_back({peers[i], {}});
            FormatAddress(s_profiles.back().device.address, peers[i]);
            changed = true;
        }
        if (changed) {
            if (!SaveProfiles()) {
                ESP_LOGW(TAG, "Imported bonds remain available for this session only");
            }
            ++s_status.revision;
        }
    }
    if (BlePageTurnerGetSnapshot().state == BlePageTurnerState::kStarting) {
        SetState(BlePageTurnerState::kIdle);
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
}

static void Reset(int reason) {
    s_synced = false;
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_report_count = 0;
    s_switching = false;
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    SetState(BlePageTurnerState::kError);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_status.enabled && !s_suspended) {
        BeginAutoLocked();
    }
}

static void HostTask(void*) {
    s_host_running = true;
    nimble_port_run();
    s_host_running = false;
    nimble_port_freertos_deinit();
}

static void InitTask(void*) {
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    // 蓝牙占用内部 RAM 前保留清理线程，确保低余量时仍可释放协议栈。
    TaskExecutor::SetRetained(TaskDomain::Cleanup, true);
    const bool cleanup_ready = TaskExecutor::Schedule([] {}, TaskDomain::Cleanup);
    const esp_err_t rc = cleanup_ready ? nimble_port_init() : ESP_ERR_NO_MEM;
    if (rc != ESP_OK) {
        TaskExecutor::SetRetained(TaskDomain::Cleanup, false);
        ESP_LOGE(TAG, "BLE initialization failed: %s", esp_err_to_name(rc));
        SetState(BlePageTurnerState::kError);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_init_retry = esp_timer_get_time() + 1000000;
        }
        s_started = false;
    } else {
        ble_npl_event_init(&s_control, Control, nullptr);
        ble_hs_cfg.sync_cb = Synced;
        ble_hs_cfg.reset_cb = Reset;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        ble_store_config_init();
        nimble_port_freertos_init(HostTask);
    }
    vTaskDelete(nullptr);
}

static bool Start(BlePageTurnerCommandHandler handler) {
    BlePageTurnerGetSnapshot();
    s_handler = handler;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || s_stopping || s_host_stopped ||
            (!s_started.load() && esp_timer_get_time() < s_init_retry)) {
            return false;
        }
        if (s_started.load()) {
            return true;
        }
        s_started = true;
        SetStateLocked(BlePageTurnerState::kStarting);
    }
    if (FirmwareCreateTask(InitTask, "pager_init", 4096, nullptr, 4, nullptr, tskNO_AFFINITY, 0) != pdPASS) {
        s_started = false;
        SetState(BlePageTurnerState::kError);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_init_retry = esp_timer_get_time() + 1000000;
        }
        return false;
    }
    return true;
}

static bool Submit(BlePageTurnerRequest request, int index, bool replace, const char* address) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || s_stopping || s_host_stopped ||
            (!replace && s_request != BlePageTurnerRequest::kNone)) {
            return false;
        }
        const bool scanning = request == BlePageTurnerRequest::kScan || request == BlePageTurnerRequest::kRefreshScan;
        if ((s_status.state == BlePageTurnerState::kConnecting || s_suspended) &&
            (scanning || request == BlePageTurnerRequest::kForget)) {
            return false;
        }
        const bool scanned = request == BlePageTurnerRequest::kConnect || request == BlePageTurnerRequest::kAutoConnect;
        const bool connecting = scanned || request == BlePageTurnerRequest::kConnectSaved;
        if (connecting && (s_suspended || s_status.state == BlePageTurnerState::kConnecting)) {
            return false;
        }
        if (request == BlePageTurnerRequest::kConnect) {
            if (!address) {
                return false;
            }
            for (index = 0; index < s_status.count; ++index) {
                if (!std::strcmp(s_status.devices[index].address, address)) {
                    break;
                }
            }
        } else if (request == BlePageTurnerRequest::kConnectSaved || request == BlePageTurnerRequest::kForget) {
            if (!address) {
                return false;
            }
            for (index = 0; index < static_cast<int>(s_profiles.size()); ++index) {
                if (!std::strcmp(s_profiles[index].device.address, address)) {
                    break;
                }
            }
        }
        if (scanned && (index < 0 || index >= s_status.count)) {
            return false;
        }
        if ((request == BlePageTurnerRequest::kConnectSaved || request == BlePageTurnerRequest::kForget) &&
            (index < 0 || index >= static_cast<int>(s_profiles.size()))) {
            return false;
        }
        if (scanned) {
            s_requested_address = s_addresses[index];
            s_requested_model = s_status.devices[index].model;
        } else if (request == BlePageTurnerRequest::kConnectSaved || request == BlePageTurnerRequest::kForget) {
            s_requested_address = s_profiles[index].address;
            s_requested_model = s_profiles[index].device.model;
        }
        if (s_request == BlePageTurnerRequest::kBind) {
            s_status.binding_pending = false;
            s_status.operation_failed = true;
            ++s_status.revision;
        }
        if (connecting) {
            if (s_status.state == BlePageTurnerState::kConnected && s_status.active_saved >= 0 &&
                !ble_addr_cmp(&s_profiles[s_status.active_saved].address, &s_requested_address)) {
                return true;
            }
            EnableLocked(true);
            if (request != BlePageTurnerRequest::kAutoConnect) {
                s_boot_pending = false;
                s_status.searching = false;
                s_auto_deadline = 0;
                s_status.timed_out = false;
            }
            FormatAddress(s_status.target_address, s_requested_address);
            s_status.target_model = s_requested_model;
            SetStateLocked(BlePageTurnerState::kConnecting);
        }
        if (scanning) {
            s_manual_discovery = true;
            s_boot_pending = false;
            s_status.searching = false;
            s_auto_deadline = 0;
            if (s_status.state == BlePageTurnerState::kError) {
                s_status.state = BlePageTurnerState::kIdle;
                ++s_status.revision;
            }
        } else if (connecting || request == BlePageTurnerRequest::kForget ||
                   request == BlePageTurnerRequest::kSuspend) {
            s_manual_discovery = false;
        }
        s_request = request;
        s_index = index;
        if (s_synced.load()) {
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
        }
    }
    return true;
}

static void QueueRelease() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_release_pending || s_release_queued || esp_timer_get_time() < s_release_retry) {
            return;
        }
        s_release_queued = true;
    }
    if (TaskExecutor::Schedule(
            []() {
                const bool released = BlePageTurnerPrepareSleep();
                std::lock_guard<std::mutex> lock(s_mutex);
                s_release_queued = false;
                if (released) {
                    s_release_pending = false;
                    s_release_retry = 0;
                } else {
                    s_release_retry = esp_timer_get_time() + 1000000;
                }
            },
            TaskDomain::Cleanup)) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_release_queued = false;
    s_release_retry = esp_timer_get_time() + 1000000;
    ESP_LOGW(TAG, "BLE release enqueue failed; retry deferred");
}

void BlePageTurnerInit(BlePageTurnerCommandHandler handler) {
    BlePageTurnerGetSnapshot();
    s_handler = handler;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    s_boot_pending = s_status.enabled;
    s_boot_not_before = esp_timer_get_time() + 10000000;
}

void BlePageTurnerPoll() {
    QueueRelease();
    BlePageTurnerRequest request = BlePageTurnerRequest::kNone;
    bool start_needed = false;
    bool persist_needed = false;
    bool scan_needed = false;
    bool release_needed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || s_suspended || s_stopping || s_host_stopped) {
            return;
        }
        const auto now = esp_timer_get_time();
        if (s_boot_pending && now >= s_boot_not_before) {
            s_boot_pending = false;
            if (s_status.enabled && s_status.state != BlePageTurnerState::kConnected) {
                BeginAutoLocked();
            }
        }
        if (s_status.enabled && s_status.searching && now >= s_auto_deadline) {
            EnableLocked(false);
            s_status.timed_out = true;
            CancelInput();
            s_manual_discovery = false;
            s_scan_pending = false;
            s_release_pending = true;
            release_needed = true;
            ESP_LOGI(TAG, "Automatic connection timed out after 30s; disabled");
        }
        start_needed = !s_started.load() && s_status.enabled && !s_boot_pending;
        persist_needed = s_enabled_dirty && !s_synced.load() && now >= s_persist_retry;
        if (request == BlePageTurnerRequest::kNone && s_synced.load() && s_request == BlePageTurnerRequest::kNone) {
            if (s_status.enabled && s_status.searching && !s_status.scanning &&
                s_status.state != BlePageTurnerState::kConnected && s_status.state != BlePageTurnerState::kConnecting &&
                now >= s_next_auto_scan) {
                s_next_auto_scan = now + 1000000;
                request = BlePageTurnerRequest::kAutoScan;
            } else if (s_enabled_dirty && now >= s_persist_retry) {
                request = BlePageTurnerRequest::kPersist;
            }
        }
    }
    if (release_needed) {
        QueueRelease();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        scan_needed = s_scan_pending;
        s_scan_pending = false;
        if (!s_status.enabled && !s_manual_discovery && !scan_needed && s_started.load() &&
            s_request == BlePageTurnerRequest::kNone) {
            s_release_pending = true;
        }
    }
    QueueRelease();
    if (scan_needed) {
        BlePageTurnerScan(s_handler.load());
        return;
    }
    if (persist_needed) {
        QueuePreferenceWrite();
    }
    if (start_needed) {
        Start(s_handler.load());
    }
    if (request != BlePageTurnerRequest::kNone) {
        Submit(request, -1, false);
    }
}

bool BlePageTurnerSetEnabled(bool enabled) {
    BlePageTurnerGetSnapshot();
    bool releasing;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_boot_pending = false;
        EnableLocked(enabled);
        s_status.timed_out = false;
        CancelInput();
        if (enabled) {
            s_suspended = false;
            if (s_status.state != BlePageTurnerState::kConnected || s_release_pending || s_stopping || s_host_stopped) {
                BeginAutoLocked();
            }
        } else {
            s_scan_pending = false;
            s_manual_discovery = false;
        }
        if (!enabled || s_stopping || s_host_stopped) {
            s_release_pending = true;
        }
        releasing = s_release_pending;
    }
    if (releasing) {
        QueueRelease();
        return true;
    }
    if (!Start(s_handler.load())) {
        return false;
    }
    return Submit(BlePageTurnerRequest::kEnable);
}

void BlePageTurnerSetStandby(bool standby) {
    BlePageTurnerGetSnapshot();
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_suspended = standby;
        if (standby) {
            s_scan_pending = false;
        }
        s_boot_pending = false;
        enabled = s_status.enabled;
        CancelInput();
        s_auto_deadline = 0;
        s_status.searching = false;
        if (!standby && enabled && s_status.state != BlePageTurnerState::kConnected) {
            BeginAutoLocked();
        }
    }
    if (standby) {
        if (s_started.load()) {
            Submit(BlePageTurnerRequest::kSuspend);
        }
    } else if (enabled) {
        Start(s_handler.load());
        Submit(BlePageTurnerRequest::kEnable);
    }
}

BlePageTurnerSnapshot BlePageTurnerGetSnapshot() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_profiles_loaded) {
        LoadProfiles();
        nvs_handle_t handle;
        if (nvs_open("pager", NVS_READONLY, &handle) == ESP_OK) {
            uint8_t enabled = 0;
            if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) {
                s_status.enabled = enabled != 0;
            }
            nvs_close(handle);
        }
        s_profiles_loaded = true;
    }
    if (!s_status.saved || s_saved_revision != s_status.revision) {
        auto saved = std::make_shared<std::vector<BlePageTurnerSavedDevice>>();
        saved->reserve(s_profiles.size());
        for (size_t i = 0; i < s_profiles.size(); ++i) {
            auto device = s_profiles[i].device;
            device.nearby =
                s_status.state == BlePageTurnerState::kConnected && s_status.active_saved == static_cast<int>(i);
            device.rssi = -127;
            saved->push_back(device);
        }
        for (int i = 0; i < s_status.count; ++i) {
            const int index = FindSaved(s_addresses[i]);
            if (index >= 0) {
                (*saved)[index].nearby = true;
                (*saved)[index].rssi = s_status.devices[i].rssi;
            }
        }
        s_status.saved = saved;
        s_saved_revision = s_status.revision;
    }
    auto snapshot = s_status;
    snapshot.saved_count = static_cast<int>(s_profiles.size());
    for (int i = 0; i < snapshot.count; ++i) {
        snapshot.devices[i].saved_index = FindSaved(s_addresses[i]);
    }
    return snapshot;
}

bool BlePageTurnerScan(BlePageTurnerCommandHandler handler, bool keep_results) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || s_stopping || s_host_stopped) {
            if (s_suspended) {
                return false;
            }
            s_handler = handler;
            s_scan_pending = true;
            return true;
        }
    }
    if (!Submit(keep_results ? BlePageTurnerRequest::kRefreshScan : BlePageTurnerRequest::kScan, -1, false)) {
        return false;
    }
    if (Start(handler)) {
        return true;
    }
    BlePageTurnerStopScan();
    return false;
}

void BlePageTurnerStopScan() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_scan_pending = false;
    if (!s_manual_discovery) {
        return;
    }
    s_manual_discovery = false;
    if (s_request == BlePageTurnerRequest::kScan || s_request == BlePageTurnerRequest::kRefreshScan) {
        s_request = BlePageTurnerRequest::kNone;
    }
    s_stop_scan_requested = true;
    if (s_synced.load() && !s_stopping && !s_host_stopped) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
    }
}

bool BlePageTurnerConnect(const char* address) {
    return s_synced.load() && Submit(BlePageTurnerRequest::kConnect, -1, true, address);
}

bool BlePageTurnerConnectSaved(const char* address) {
    return s_synced.load() && Submit(BlePageTurnerRequest::kConnectSaved, -1, true, address);
}

bool BlePageTurnerForget(const char* address) {
    return s_synced.load() && Submit(BlePageTurnerRequest::kForget, -1, true, address);
}

bool BlePageTurnerSetLearning(bool enabled) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (enabled && (s_release_pending || s_status.state != BlePageTurnerState::kConnected || s_status.binding_pending ||
                    s_status.active_saved < 0 ||
                    !BlePageTurnerSupportsButtonLearning(s_profiles[s_status.active_saved].device.model))) {
        return false;
    }
    if (!enabled && !s_status.learning) {
        return true;
    }
    CancelInput();
    s_status.learning = enabled;
    if (enabled) {
        s_status.operation_failed = false;
    }
    s_status.learned_key = 0;
    ++s_status.revision;
    return true;
}

bool BlePageTurnerBind(BlePageTurnerAction action, uint8_t key) {
    const auto index = static_cast<unsigned>(action);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_release_pending || index >= 2 || key > 2 || s_status.state != BlePageTurnerState::kConnected ||
            s_status.active_saved < 0 || !s_status.learning || s_status.binding_pending ||
            s_request != BlePageTurnerRequest::kNone || !s_synced.load()) {
            return false;
        }
        s_requested_address = s_profiles[s_status.active_saved].address;
        s_binding_key = key;
        s_index = index;
        s_request = BlePageTurnerRequest::kBind;
        s_status.binding_pending = true;
        s_status.operation_failed = false;
        ++s_status.revision;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
    }
    return true;
}

void BlePageTurnerDisconnect() {
    BlePageTurnerSetEnabled(false);
}

bool BlePageTurnerPrepareSleep() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_started.load()) {
            return true;
        }
        if (s_stopping || (!s_host_running.load() && !s_host_stopped)) {
            return false;
        }
        CancelInput();
        s_stopping = true;
        s_synced = false;
        s_request = BlePageTurnerRequest::kNone;
        s_manual_discovery = false;
        s_stop_scan_requested = false;
    }
    int rc = 0;
    if (!s_host_stopped) {
        rc = nimble_port_stop();
        if (!rc) {
            s_host_stopped = true;
            ble_npl_event_deinit(&s_control);
        }
    }
    if (!rc) {
        rc = nimble_port_deinit();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_stopping = false;
    if (rc) {
        ESP_LOGW(TAG, "BLE stack release failed: %d", rc);
        if (!s_host_stopped) {
            s_synced = ble_hs_synced();
        }
        s_status.state = BlePageTurnerState::kError;
        ++s_status.revision;
        return false;
    }
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_report_count = 0;
    s_switching = false;
    s_auto_scan = false;
    s_status.state = BlePageTurnerState::kIdle;
    s_status.scanning = false;
    s_status.active_saved = -1;
    s_status.learning = false;
    s_status.learned_key = 0;
    s_status.operation_failed = s_status.binding_pending;
    s_status.binding_pending = false;
    s_status.count = 0;
    if (!s_status.enabled) {
        s_status.searching = false;
    }
    ++s_status.revision;
    s_control = {};
    s_host_stopped = false;
    s_started = false;
    TaskExecutor::SetRetained(TaskDomain::Cleanup, false);
    ESP_LOGI(TAG, "BLE stack released");
    return true;
}
