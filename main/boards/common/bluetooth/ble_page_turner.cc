#include "ble_page_turner.h"
#include "free2_buttons.h"
#include "free3r_buttons.h"
#include "smart1e_buttons.h"
#include "hbtr_gesture_buttons.h"
#include "application.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <nvs.h>
#include <esp_timer.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace BlePageTurner {
namespace {
constexpr const char* TAG = "BlePageTurner";
constexpr bool IsHbtrGestureModel(Model model) {
    return model == Model::kHbtr002X || model == Model::kHbtr003X;
}
const ble_uuid16_t kService = BLE_UUID16_INIT(0x1812);
const ble_uuid16_t kReport = BLE_UUID16_INIT(0x2a4d);
std::mutex s_mutex;
Snapshot s_status;
ble_addr_t s_addresses[kMaxDevices];
struct Profile {
    ble_addr_t address{}; // 保存的 BLE 身份地址
    SavedDevice device; // 型号及翻页按键绑定
};
std::vector<Profile> s_profiles;
uint32_t s_saved_revision = UINT32_MAX;
bool s_initialized = false;
bool s_boot_pending = false;
int64_t s_boot_not_before = 0;
bool s_profiles_loaded = false; // 由 s_mutex 保护，配置只加载一次
std::atomic<CommandHandler> s_handler{nullptr};
std::atomic<bool> s_started{false};
std::atomic<bool> s_synced{false};
ble_npl_event s_control;
uint8_t s_pressed_key = 0; // 过滤同一协议按压中的重复非零报告
HbtrGestureButtons::Decoder s_hbtr_gesture_buttons;
enum class Request {
    kBind, // 保存学习得到的按键绑定
    kAutoScan, // 自动连接扫描
    kAutoConnect, // 自动寻找过程中连接候选设备
    kEnable, // 开启自动连接
    kDisable, // 关闭并断开
    kSuspend, // 待机暂停，保留开关
    kPersist, // 保存开关
    kNone, // 无待处理请求
    kScan, // 扫描设备
    kRefreshScan, // 保留结果的后台扫描
    kConnect, // 连接所选设备
    kConnectSaved, // 连接保存的身份地址
    kForget, // 清除保存记录及配对密钥
};
Request s_request = Request::kNone;
int s_index = -1;
ble_addr_t s_requested_address{};
Model s_requested_model = Model::kFree2;
uint8_t s_binding_key = 0;
bool s_switching = false;
ble_addr_t s_switch_address{};
Model s_switch_model = Model::kFree2;
uint8_t s_own_address = 0;
Model s_model = Model::kFree2;
uint16_t s_connection = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_service_start = 0, s_service_end = 0;
struct Report {
    uint16_t value = 0; // 通知值句柄
    uint16_t end = 0; // 特征描述符范围末尾
    uint16_t cccd = 0; // 客户端通知配置句柄
    uint16_t reference = 0; // Report Reference 描述符句柄
    uint8_t id = 0; // HID 报告编号，用于订阅日志
    bool buttons = false; // 已验证按键协议的输入报告
};
Report s_reports[8];
int s_report_count = 0, s_subscribing = 0;
int s_input_reports = 0;
int s_active_button_report = -1;
State s_after_disconnect = State::kIdle;
bool s_cancelled = false;
bool s_pairing = false;
bool s_pairing_waiting = false;
bool s_pairing_reference = false;
std::atomic<bool> s_stopping{false};
std::atomic<bool> s_host_stopped{false};
std::atomic<bool> s_suspended{false};
bool s_auto_scan = false;
bool s_manual_discovery = false; // 由 s_mutex 保护，设备管理持有扫描
bool s_stop_scan_requested = false; // 由 BLE 任务取消扫描，不覆盖连接等请求
bool s_scan_seen[kMaxDevices] = {};
bool s_enabled_dirty = false;
std::mutex s_persist_mutex;
std::atomic<bool> s_preference_queued{false};
int64_t s_auto_deadline = 0;
int64_t s_next_auto_scan = 0;
int64_t s_persist_retry = 0;
int64_t s_init_retry = 0;
ble_addr_t s_auto_tried[kMaxDevices];
int s_auto_tried_count = 0;
constexpr int64_t kAutoTimeoutUs = 30000000;
extern "C" void ble_store_config_init();

int GapEvent(ble_gap_event* event, void*);
void Fail(int error);
void StopActivities();
bool Submit(Request request, int index = -1, bool replace = true, const char* address = nullptr);
void StartScan(bool automatic = false, bool keep_results = false);
void BeginAutoLocked() {
    s_status.searching = true;
    s_status.timed_out = false;
    s_auto_deadline = esp_timer_get_time() + kAutoTimeoutUs;
    s_next_auto_scan = 0;
    s_auto_tried_count = 0;
}
void EnableLocked(bool enabled) {
    if (s_status.enabled != enabled) s_enabled_dirty = true;
    s_status.enabled = enabled;
    if (!enabled) {
        s_auto_deadline = 0;
        s_status.searching = false;
    }
    ++s_status.revision;
}
void PersistEnabled() {
    std::lock_guard<std::mutex> persistence_lock(s_persist_mutex);
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_enabled_dirty) return;
        enabled = s_status.enabled;
    }
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("pager", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_u8(handle, "enabled", enabled);
        if (rc == ESP_OK) rc = nvs_commit(handle);
        nvs_close(handle);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (rc == ESP_OK && s_status.enabled == enabled) s_enabled_dirty = false;
    else if (rc != ESP_OK) ESP_LOGW(TAG, "Save enabled failed: %s", esp_err_to_name(rc));
    s_persist_retry = esp_timer_get_time() + 1000000;
}
void QueuePreferenceWrite() {
    if (s_preference_queued.exchange(true)) return;
    if (!Application::GetInstance().Schedule([]() {
        PersistEnabled();
        s_preference_queued = false;
    }, Application::TaskDomain::Storage)) {
        s_preference_queued = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_persist_retry = esp_timer_get_time() + 1000000;
    }
}

int Descriptor(uint16_t connection, const ble_gatt_error* error, uint16_t,
               const ble_gatt_dsc* descriptor, void*);

void CancelInput() {
    s_hbtr_gesture_buttons.Reset();
    if (s_pressed_key) ESP_LOGD(TAG, "button=%c phase=cancel", 'A' + s_pressed_key - 1);
    s_pressed_key = 0;
    if (auto handler = s_handler.load()) handler(Action::kPrevious, KeyPhase::kCancel);
}

// 调用方持有 s_mutex，保证解码、取消及输入分发不会交错。
void InputLocked(uint8_t key) {
    if (key == s_pressed_key) return;
    s_pressed_key = key;
    if (!key) return;
    if (s_status.learning) {
        if (s_status.binding_pending || key == Free3RButtons::kStandbyKey) return;
        s_status.learned_key = key;
        ++s_status.revision;
        return;
    }
    if (s_status.active_saved < 0) return;
    if ((s_model == Model::kFree3R || s_model == Model::kSmart1E || IsHbtrGestureModel(s_model)) &&
        key == Free3RButtons::kStandbyKey) {
        ESP_LOGD(TAG, "button=C action=standby phase=click");
        if (auto handler = s_handler.load()) handler(Action::kStandby, KeyPhase::kClick);
        return;
    }
    const auto& bindings = s_profiles[s_status.active_saved].device.keys;
    const int action = key == bindings[0] ? 0 : key == bindings[1] ? 1 : -1;
    if (action < 0) return;
    ESP_LOGD(TAG, "button=%c action=%s phase=click", 'A' + key - 1, action == 0 ? "previous" : "next");
    if (auto handler = s_handler.load()) handler(static_cast<Action>(action), KeyPhase::kClick);
}

void FormatAddress(char* text, const ble_addr_t& address) {
    const auto* a = address.val;
    std::snprintf(text, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// 固定字节布局避免持久化结构体填充；写入成功后再发布配置变化。
bool SaveProfiles() {
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
        if (rc == ESP_OK) rc = nvs_commit(handle);
        nvs_close(handle);
    }
    if (rc != ESP_OK) ESP_LOGW(TAG, "Save devices failed: %s", esp_err_to_name(rc));
    return rc == ESP_OK;
}

void LoadProfiles() {
    nvs_handle_t handle;
    if (nvs_open("pager", NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0;
    esp_err_t rc = nvs_get_blob(handle, "devices", nullptr, &length);
    if (rc != ESP_OK || !length) { nvs_close(handle); return; }
    std::vector<uint8_t> bytes(length);
    rc = nvs_get_blob(handle, "devices", bytes.data(), &length);
    nvs_close(handle);
    if (rc != ESP_OK || !length || bytes[0] < 1 || bytes[0] > 5) return;
    const bool legacy = bytes[0] < 5;
    const size_t stride = bytes[0] == 3 ? 9 : 10;
    const size_t offset = legacy ? 2 : 1;
    if (length < offset || (length - offset) % stride) return;
    size_t count = (length - offset) / stride;
    if (legacy) {
        constexpr size_t kLegacySlots = 3; // 旧格式固定槽位，仅用于迁移
        if (count != kLegacySlots || bytes[1] > kLegacySlots) return;
        count = bytes[1];
    }
    for (size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + offset + i * stride;
        if (record[0] > BLE_ADDR_RANDOM_ID || (record[7] && record[7] == record[8]) ||
            (bytes[0] >= 4 && record[9] >= static_cast<uint8_t>(Model::kCount)) ||
            (!legacy && (record[7] > 2 || record[8] > 2))) return;
        for (size_t j = 0; j < i; ++j) {
            const auto* previous = bytes.data() + offset + j * stride;
            if (record[0] == previous[0] && !std::memcmp(record + 1, previous + 1, 6)) return;
        }
    }
    s_profiles.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + offset + i * stride;
        auto& profile = s_profiles[i];
        profile.address.type = record[0];
        std::memcpy(profile.address.val, record + 1, 6);
        FormatAddress(profile.device.address, profile.address);
        profile.device.model = bytes[0] >= 4 ? static_cast<Model>(record[9]) : Model::kFree2;
        for (int action = 0; action < 2; ++action)
            profile.device.keys[action] = record[7 + action] <= 2 ? record[7 + action] : 0;
    }
    if (legacy) SaveProfiles();
}

int FindSaved(const ble_addr_t& address) {
    for (size_t i = 0; i < s_profiles.size(); ++i)
        if (!ble_addr_cmp(&s_profiles[i].address, &address)) return static_cast<int>(i);
    return -1;
}

bool RememberConnected() {
    ble_gap_conn_desc description{};
    if (ble_gap_conn_find(s_connection, &description)) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_status.enabled || s_suspended) return false;
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
        if (added) s_profiles.pop_back();
        else s_profiles[index].device.model = previous_model;
        return false;
    }
    s_status.active_saved = index;
    s_auto_deadline = 0;
    s_status.searching = false;
    ++s_status.revision;
    return true;
}

void DiscoverDescriptor() {
    const auto& report = s_reports[s_subscribing];
    const int rc = ble_gattc_disc_all_dscs(s_connection, report.value, report.end, Descriptor, nullptr);
    if (rc) Fail(rc);
}

void SetStateLocked(State state) {
    if (state != State::kConnected) CancelInput();
    s_status.state = state;
    if (state != State::kConnected) {
        s_status.learning = false;
        s_status.learned_key = 0;
        s_status.active_saved = -1;
        if (s_status.binding_pending) { s_status.binding_pending = false; s_status.operation_failed = true; }
    }
    ++s_status.revision;
}

void SetState(State state) {
    std::lock_guard<std::mutex> lock(s_mutex);
    SetStateLocked(state);
}

void Fail(int error) {
    ESP_LOGW(TAG, "BLE operation failed: %d", error);
    s_after_disconnect = State::kError;
    SetState(State::kError);
    if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int Subscribed(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr*, void*);
int Referenced(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attr, void*);

void ReadReference() {
    const int rc = ble_gattc_read(s_connection, s_reports[s_subscribing].reference, Referenced, nullptr);
    if (rc) Fail(rc);
}

void EnableNotifications() {
    const uint8_t notify[] = {1, 0};
    const int rc = ble_gattc_write_flat(s_connection, s_reports[s_subscribing].cccd,
        notify, sizeof(notify), Subscribed, nullptr);
    if (rc) Fail(rc);
}

bool RetryWithEncryption(uint16_t connection, int status, bool reference) {
    if (s_pairing || (status != BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) &&
        status != BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC))) return false;
    s_pairing = true;
    s_pairing_waiting = true;
    s_pairing_reference = reference;
    const int rc = ble_gap_security_initiate(connection);
    if (rc) Fail(rc);
    return true;
}

void NextReport() {
    if (++s_subscribing < s_report_count) DiscoverDescriptor();
    else if (!s_input_reports) Fail(BLE_HS_ENOTSUP);
    else if (!RememberConnected()) {
        if (!GetSnapshot().enabled || s_suspended) StopActivities();
        else Fail(BLE_HS_ESTORE_CAP);
    }
    else {
        SetState(State::kConnected);
        PersistEnabled();
        ESP_LOGI(TAG, "%s input reports ready, reports=%d", ModelName(s_model), s_input_reports);
    }
}

int Subscribed(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    if (connection != s_connection) return 0;
    if (RetryWithEncryption(connection, error->status, false)) return 0;
    if (error->status) Fail(error->status);
    else {
        ESP_LOGI(TAG, "Subscribed: report=%u handle=%u cccd=%u", s_reports[s_subscribing].id,
            s_reports[s_subscribing].value, s_reports[s_subscribing].cccd);
        NextReport();
    }
    return 0;
}

int Referenced(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
    if (connection != s_connection) return 0;
    if (RetryWithEncryption(connection, error->status, true)) return 0;
    if (error->status) { Fail(error->status); return 0; }
    uint8_t reference[2];
    if (!attr || !attr->om || OS_MBUF_PKTLEN(attr->om) != sizeof(reference) ||
        os_mbuf_copydata(attr->om, 0, sizeof(reference), reference)) {
        Fail(BLE_HS_EBADDATA);
    } else {
        ESP_LOGI(TAG, "Report reference: model=%s handle=%u id=%u type=%u cccd=%u", ModelName(s_model),
            s_reports[s_subscribing].value, reference[0], reference[1], s_reports[s_subscribing].cccd);
        if (reference[1] == Free2Buttons::kInputReport) {
            s_reports[s_subscribing].id = reference[0];
            if (!s_reports[s_subscribing].cccd) {
                Fail(BLE_HS_EBADDATA);
            } else {
                s_reports[s_subscribing].buttons =
                    (s_model == Model::kFree2 && Free2Buttons::IsButtonReport(reference[0], reference[1])) ||
                    (s_model == Model::kFree3R && Free3RButtons::IsButtonReport(reference[0], reference[1])) ||
                    (s_model == Model::kSmart1E && Smart1EButtons::IsButtonReport(reference[0], reference[1])) ||
                    (IsHbtrGestureModel(s_model) && HbtrGestureButtons::IsButtonReport(reference[0], reference[1]));
                ++s_input_reports;
                EnableNotifications();
            }
        } else NextReport();
    }
    return 0;
}

int Descriptor(uint16_t connection, const ble_gatt_error* error, uint16_t,
               const ble_gatt_dsc* descriptor, void*) {
    if (connection != s_connection) return 0;
    auto& report = s_reports[s_subscribing];
    if (!error->status) {
        const uint16_t uuid = ble_uuid_u16(&descriptor->uuid.u);
        if (uuid == 0x2902) report.cccd = descriptor->handle;
        else if (uuid == 0x2908) report.reference = descriptor->handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (report.reference) ReadReference();
        else NextReport();
    } else Fail(error->status);
    return 0;
}

int Characteristic(uint16_t connection, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
    if (connection != s_connection) return 0;
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
        if (!s_reports[s_report_count - 1].end) s_reports[s_report_count - 1].end = s_service_end;
        s_subscribing = 0;
        DiscoverDescriptor();
    } else {
        Fail(error->status);
    }
    return 0;
}

int Service(uint16_t connection, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (connection != s_connection) return 0;
    if (!error->status && !s_service_start) {
        s_service_start = service->start_handle;
        s_service_end = service->end_handle;
    } else if (error->status == BLE_HS_EDONE && s_service_start) {
        const int rc = ble_gattc_disc_all_chrs(connection, s_service_start, s_service_end, Characteristic, nullptr);
        if (rc) Fail(rc);
    } else if (error->status) {
        Fail(error->status);
    }
    return 0;
}

void StartScan(bool automatic, bool keep_results) {
    if (ble_gap_conn_active() || s_suspended) return;
    if (ble_gap_disc_active()) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!keep_results) s_status.count = 0;
        std::memset(s_scan_seen, 0, sizeof(s_scan_seen));
        s_status.scanning = true;
        if (s_status.state == State::kError) s_status.state = State::kIdle;
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
        { std::lock_guard<std::mutex> lock(s_mutex); s_status.scanning = false; }
        Fail(rc);
    }
}

void TryAutomaticConnection() {
    int candidate = -1;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_status.enabled || !s_status.searching || s_suspended || s_auto_deadline <= esp_timer_get_time()) return;
        bool candidate_saved = false;
        for (int i = 0; i < s_status.count; ++i) {
            bool tried = false;
            for (int j = 0; j < s_auto_tried_count; ++j) if (!ble_addr_cmp(&s_auto_tried[j], &s_addresses[i])) tried = true;
            if (tried) continue;
            const bool saved = FindSaved(s_addresses[i]) >= 0;
            if (candidate < 0 || (saved && !candidate_saved) ||
                (saved == candidate_saved && s_status.devices[i].rssi > s_status.devices[candidate].rssi)) {
                candidate = i;
                candidate_saved = saved;
            }
        }
        s_next_auto_scan = esp_timer_get_time() + 1000000;
        if (candidate >= 0 && s_auto_tried_count < kMaxDevices) s_auto_tried[s_auto_tried_count++] = s_addresses[candidate];
        else if (candidate < 0) s_auto_tried_count = 0;
    }
    if (candidate >= 0) Submit(Request::kAutoConnect, candidate, false);
}

void StopActivities() {
    s_switching = false;
    s_cancelled = true;
    s_after_disconnect = State::kIdle;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    if (ble_gap_conn_active()) ble_gap_conn_cancel();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.scanning = false;
    }
    if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
        if (rc && rc != BLE_HS_EALREADY) Fail(rc);
    } else SetState(State::kIdle);
}

void ConnectTo(const ble_addr_t& address, Model model) {
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    s_after_disconnect = State::kIdle;
    s_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_suspended || s_request == Request::kDisable || s_request == Request::kSuspend) return;
        s_status.scanning = false;
        EnableLocked(true);
        FormatAddress(s_status.target_address, address);
        s_model = model;
        s_status.target_model = model;
        s_status.state = State::kConnecting;
        ++s_status.revision;
    }
    PersistEnabled();
    const int rc = ble_gap_connect(s_own_address, &address, 10000, nullptr, GapEvent, nullptr);
    if (rc) Fail(rc);
}

void Control(ble_npl_event*) {
    Request request;
    int index;
    ble_addr_t address{};
    Model model;
    bool stop_scan;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        request = s_request;
        index = s_index;
        s_request = Request::kNone;
        address = s_requested_address;
        model = s_requested_model;
        stop_scan = s_stop_scan_requested;
        s_stop_scan_requested = false;
        if (request == Request::kConnectSaved || request == Request::kForget) {
            index = FindSaved(address);
        }
    }
    if (stop_scan) {
        if (ble_gap_disc_active()) {
            const int rc = ble_gap_disc_cancel();
            if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "Cancel scan failed: %d", rc);
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.scanning = false;
        ++s_status.revision;
        ESP_LOGI(TAG, "Device manager scan stopped");
    }
    if ((request == Request::kConnectSaved || request == Request::kForget) && index < 0) return;
    PersistEnabled();
    if (request == Request::kBind) {
        std::lock_guard<std::mutex> lock(s_mutex);
        const int saved = FindSaved(address);
        bool success = saved >= 0 && s_status.state == State::kConnected && s_status.active_saved == saved;
        if (success) {
            auto& device = s_profiles[saved].device;
            const auto before = device;
            const uint8_t key = s_binding_key;
            if (key && device.keys[1 - index] == key) device.keys[1 - index] = 0;
            device.keys[index] = key;
            success = SaveProfiles();
            if (!success) device = before;
            else {
                ESP_LOGI(TAG, "Binding saved: action=%s key=%u", index == 0 ? "previous" : "next", key);
                CancelInput();
                s_status.learning = false;
                s_status.learned_key = 0;
            }
        }
        s_status.binding_pending = false;
        s_status.operation_failed = !success;
        ++s_status.revision;
    } else if (request == Request::kDisable || request == Request::kSuspend) {
        StopActivities();
    } else if (request == Request::kEnable || request == Request::kAutoScan) {
        if (GetSnapshot().enabled && !s_suspended && s_connection == BLE_HS_CONN_HANDLE_NONE) StartScan(true);
    } else if (request == Request::kScan || request == Request::kRefreshScan) {
        if (ble_gap_disc_active() && (s_auto_scan || request == Request::kScan)) {
            const int rc = ble_gap_disc_cancel();
            if (rc && rc != BLE_HS_EALREADY) { Fail(rc); return; }
        }
        StartScan(false, request == Request::kRefreshScan);
    } else if (request == Request::kConnect || request == Request::kConnectSaved || request == Request::kAutoConnect) {
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        if (ble_gap_conn_active()) return;
        if (s_connection != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_conn_desc connected{};
            if (!ble_gap_conn_find(s_connection, &connected) && !ble_addr_cmp(&connected.peer_id_addr, &address)) return;
            s_switch_address = address;
            s_switch_model = model;
            s_switching = true;
            s_after_disconnect = State::kIdle;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                EnableLocked(true);
                FormatAddress(s_status.target_address, address);
                s_status.target_model = model;
            }
            SetState(State::kConnecting);
            const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
            if (rc) { s_switching = false; Fail(rc); }
        } else ConnectTo(address, model);
    } else if (request == Request::kForget) {
        if (ble_gap_conn_active()) return;
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        s_after_disconnect = State::kIdle;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_status.active_saved == index) { EnableLocked(false); s_switching = false; }
            s_status.scanning = false;
        }
        PersistEnabled();
        const int rc = ble_gap_unpair(&address);
        if (rc && rc != BLE_HS_ENOENT) { Fail(rc); return; }
        std::lock_guard<std::mutex> lock(s_mutex);
        CancelInput();
        const auto removed = s_profiles[index];
        s_profiles.erase(s_profiles.begin() + index);
        if (!SaveProfiles()) {
            s_profiles.insert(s_profiles.begin() + index, removed);
            s_status.state = State::kError;
        } else {
            if (s_status.active_saved == index) s_status.active_saved = -1;
            else if (s_status.active_saved > index) --s_status.active_saved;
        }
        ++s_status.revision;
    }
}

int GapEvent(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            ble_hs_adv_fields fields{};
            const auto& found = event->disc;
            if (ble_hs_adv_parse_fields(&fields, found.data, found.length_data) || !fields.name) break;
            int model = 0;
            for (; model < static_cast<int>(Model::kCount); ++model) {
                const char* name = ModelName(static_cast<Model>(model));
                if (fields.name_len == std::strlen(name) && !std::memcmp(fields.name, name, fields.name_len)) break;
            }
            if (model == static_cast<int>(Model::kCount)) break;
            std::lock_guard<std::mutex> lock(s_mutex);
            int index = 0;
            while (index < s_status.count && ble_addr_cmp(&s_addresses[index], &found.addr)) ++index;
            if (index == kMaxDevices) break;
            s_scan_seen[index] = true;
            if (index == s_status.count) {
                s_addresses[index] = found.addr;
                s_status.devices[index].model = static_cast<Model>(model);
                FormatAddress(s_status.devices[index].address, found.addr);
                ESP_LOGI(TAG, "Found %s address=%s", ModelName(s_status.devices[index].model), s_status.devices[index].address);
                ++s_status.count;
                ++s_status.revision;
            }
            if (s_status.devices[index].rssi != found.rssi) {
                s_status.devices[index].rssi = found.rssi;
                ++s_status.revision;
            }
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (!s_auto_scan) {
                    int kept = 0;
                    for (int i = 0; i < s_status.count; ++i) if (s_scan_seen[i]) {
                        s_addresses[kept] = s_addresses[i];
                        s_status.devices[kept++] = s_status.devices[i];
                    }
                    s_status.count = kept;
                }
                s_status.scanning = false;
                ++s_status.revision;
                ESP_LOGD(TAG, "Scan complete: count=%d auto=%d reason=%d", s_status.count, s_auto_scan,
                    event->disc_complete.reason);
            }
            if (s_auto_scan && s_connection == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) TryAutomaticConnection();
            break;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status) {
                if (!s_cancelled) ESP_LOGW(TAG, "Connect failed: %d", event->connect.status);
                SetState(s_cancelled ? State::kIdle : State::kError);
                break;
            }
            s_connection = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected, handle=%u; discovering HID reports", s_connection);
            if (s_cancelled) {
                const int rc = ble_gap_terminate(s_connection, BLE_ERR_REM_USER_CONN_TERM);
                if (rc) Fail(rc);
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
                if (rc) Fail(rc);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected, handle=%u reason=%d", event->disconnect.conn.conn_handle, event->disconnect.reason);
            s_connection = BLE_HS_CONN_HANDLE_NONE;
            s_report_count = 0;
            SetState(s_after_disconnect);
            if (s_switching) {
                s_switching = false;
                if (GetSnapshot().enabled && !s_suspended) ConnectTo(s_switch_address, s_switch_model);
            } else {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (s_after_disconnect != State::kError && s_status.enabled && !s_suspended && !s_stopping && !s_status.searching)
                    BeginAutoLocked();
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.conn_handle == s_connection && s_pairing_waiting) {
                s_pairing_waiting = false;
                if (event->enc_change.status) Fail(event->enc_change.status);
                else if (s_pairing_reference) ReadReference();
                else EnableNotifications();
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            const auto& report = event->notify_rx;
            const int length = OS_MBUF_PKTLEN(report.om);
            if (ESP_LOG_ENABLED(ESP_LOG_DEBUG) && esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
                int report_id = -1;
                for (int i = 0; i < s_report_count; ++i)
                    if (s_reports[i].value == report.attr_handle) { report_id = s_reports[i].id; break; }
                if (!length) ESP_LOGD(TAG, "Notify conn=%u handle=%u report=%d len=0", report.conn_handle, report.attr_handle, report_id);
                for (int offset = 0; offset < length; offset += 16) {
                    uint8_t raw[16];
                    const int copied = length - offset < 16 ? length - offset : 16;
                    char text[sizeof(raw) * 3 + 1] = {};
                    if (os_mbuf_copydata(report.om, offset, copied, raw)) {
                        ESP_LOGW(TAG, "Notify read failed, handle=%u len=%d offset=%d", report.attr_handle, length, offset);
                        break;
                    }
                    for (int i = 0; i < copied; ++i) std::snprintf(text + 3 * i, 4, "%02x ", raw[i]);
                    ESP_LOGD(TAG, "Notify conn=%u handle=%u report=%d len=%d offset=%d data=%s", report.conn_handle,
                        report.attr_handle, report_id, length, offset, text);
                }
            }
            if (report.conn_handle != s_connection || !s_input_reports ||
                GetSnapshot().state != State::kConnected || length < 1 || length > 4) break;
            int index = 0;
            while (index < s_report_count && (s_reports[index].value != report.attr_handle || !s_reports[index].buttons)) ++index;
            if (index == s_report_count) break;
            uint8_t bytes[4];
            if (os_mbuf_copydata(report.om, 0, length, bytes)) {
                ESP_LOGW(TAG, "Input read failed: handle=%u len=%d", report.attr_handle, length);
                break;
            }
            std::lock_guard<std::mutex> lock(s_mutex);
            if (IsHbtrGestureModel(s_model)) {
                const auto key = s_hbtr_gesture_buttons.Decode(s_reports[index].id, bytes, length);
                if (key != HbtrGestureButtons::kInvalid) { InputLocked(key); InputLocked(0); }
                break;
            }
            const uint8_t key = s_model == Model::kSmart1E ? Smart1EButtons::Decode(bytes, length) :
                s_model == Model::kFree3R ? Free3RButtons::Decode(bytes, length) :
                Free2Buttons::Decode(s_reports[index].id, bytes, length);
            if (key == Free2Buttons::kInvalid) break;
            if (s_active_button_report < 0 && key) {
                s_active_button_report = index;
                ESP_LOGI(TAG, "Button input selected: report=%u handle=%u", s_reports[index].id, report.attr_handle);
            }
            // 同一连接固定使用首个有效按键通道，避免其他报告的零值干扰去重。
            if (index == s_active_button_report) {
                InputLocked(key);
                // 单字节方向指令没有松开通知，立即结束内部按压状态以保留连续同向短按。
                if ((s_model == Model::kFree2 && s_reports[index].id == Free2Buttons::kClickReportId) ||
                    (s_model == Model::kSmart1E && s_reports[index].id == Smart1EButtons::kClickReportId)) InputLocked(0);
            }
            break;
        }
        default: break;
    }
    return 0;
}

void Synced() {
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &s_own_address);
    if (rc) { Fail(rc); return; }
    s_synced = true;
    // 导入本模块此前已建立的配对，使升级前的设备也可在管理页重连或忘记。
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (!ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            if (FindSaved(peers[i]) >= 0) continue;
            s_profiles.push_back({peers[i], {}});
            FormatAddress(s_profiles.back().device.address, peers[i]);
            changed = true;
        }
        if (changed) {
            if (!SaveProfiles()) ESP_LOGW(TAG, "Imported bonds remain available for this session only");
            ++s_status.revision;
        }
    }
    if (GetSnapshot().state == State::kStarting) SetState(State::kIdle);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
}

void Reset(int reason) {
    s_synced = false;
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_report_count = 0;
    s_switching = false;
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    SetState(State::kError);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_status.enabled && !s_suspended) BeginAutoLocked();
}

void HostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void InitTask(void*) {
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    const esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "BLE initialization failed: %s", esp_err_to_name(rc));
        SetState(State::kError);
        { std::lock_guard<std::mutex> lock(s_mutex); s_init_retry = esp_timer_get_time() + 1000000; }
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

bool Start(CommandHandler handler) {
    GetSnapshot();
    s_handler = handler;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_stopping || s_host_stopped || (!s_started.load() && esp_timer_get_time() < s_init_retry)) return false;
    }
    bool expected = false;
    if (s_started.compare_exchange_strong(expected, true)) {
        SetState(State::kStarting);
        if (xTaskCreate(InitTask, "pager_init", 4096, nullptr, 4, nullptr) != pdPASS) {
            s_started = false;
            SetState(State::kError);
            { std::lock_guard<std::mutex> lock(s_mutex); s_init_retry = esp_timer_get_time() + 1000000; }
            return false;
        }
    }
    return true;
}

bool Submit(Request request, int index, bool replace, const char* address) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_stopping || s_host_stopped || (!replace && s_request != Request::kNone)) return false;
        const bool scanning = request == Request::kScan || request == Request::kRefreshScan;
        if ((s_status.state == State::kConnecting || s_suspended) && (scanning || request == Request::kForget)) return false;
        const bool scanned = request == Request::kConnect || request == Request::kAutoConnect;
        const bool connecting = scanned || request == Request::kConnectSaved;
        if (connecting && (s_suspended || s_status.state == State::kConnecting)) return false;
        if (request == Request::kConnect) {
            if (!address) return false;
            for (index = 0; index < s_status.count; ++index)
                if (!std::strcmp(s_status.devices[index].address, address)) break;
        } else if (request == Request::kConnectSaved || request == Request::kForget) {
            if (!address) return false;
            for (index = 0; index < static_cast<int>(s_profiles.size()); ++index)
                if (!std::strcmp(s_profiles[index].device.address, address)) break;
        }
        if (scanned && (index < 0 || index >= s_status.count)) return false;
        if ((request == Request::kConnectSaved || request == Request::kForget) &&
            (index < 0 || index >= static_cast<int>(s_profiles.size()))) return false;
        if (scanned) {
            s_requested_address = s_addresses[index];
            s_requested_model = s_status.devices[index].model;
        } else if (request == Request::kConnectSaved || request == Request::kForget) {
            s_requested_address = s_profiles[index].address;
            s_requested_model = s_profiles[index].device.model;
        }
        if (s_request == Request::kBind) {
            s_status.binding_pending = false;
            s_status.operation_failed = true;
            ++s_status.revision;
        }
        if (connecting) {
            if (s_status.state == State::kConnected && s_status.active_saved >= 0 &&
                !ble_addr_cmp(&s_profiles[s_status.active_saved].address, &s_requested_address)) return true;
            EnableLocked(true);
            if (request != Request::kAutoConnect) {
                s_boot_pending = false;
                s_status.searching = false;
                s_auto_deadline = 0;
                s_status.timed_out = false;
            }
            FormatAddress(s_status.target_address, s_requested_address);
            s_status.target_model = s_requested_model;
            SetStateLocked(State::kConnecting);
        }
        if (scanning) {
            s_manual_discovery = true;
            s_boot_pending = false;
            s_status.searching = false;
            s_auto_deadline = 0;
            if (s_status.state == State::kError) {
                s_status.state = State::kIdle;
                ++s_status.revision;
            }
        } else if (connecting || request == Request::kForget || request == Request::kDisable || request == Request::kSuspend) {
            s_manual_discovery = false;
        }
        s_request = request;
        s_index = index;
        if (s_synced.load()) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
    }
    return true;
}
} // namespace

Snapshot GetSnapshot() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_profiles_loaded) {
        LoadProfiles();
        nvs_handle_t handle;
        if (nvs_open("pager", NVS_READONLY, &handle) == ESP_OK) {
            uint8_t enabled = 0;
            if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) s_status.enabled = enabled != 0;
            nvs_close(handle);
        }
        s_profiles_loaded = true;
    }
    if (!s_status.saved || s_saved_revision != s_status.revision) {
        auto saved = std::make_shared<std::vector<SavedDevice>>();
        saved->reserve(s_profiles.size());
        for (size_t i = 0; i < s_profiles.size(); ++i) {
            auto device = s_profiles[i].device;
            device.nearby = s_status.state == State::kConnected && s_status.active_saved == static_cast<int>(i);
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
    for (int i = 0; i < snapshot.count; ++i) snapshot.devices[i].saved_index = FindSaved(s_addresses[i]);
    return snapshot;
}

void Init(CommandHandler handler) {
    GetSnapshot();
    s_handler = handler;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) return;
    s_initialized = true;
    s_boot_pending = s_status.enabled;
    s_boot_not_before = esp_timer_get_time() + 10000000;
}

bool SetEnabled(bool enabled) {
    GetSnapshot();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_boot_pending = false;
        EnableLocked(enabled);
        s_status.timed_out = false;
        CancelInput();
        if (enabled) {
            s_suspended = false;
            if (s_status.state != State::kConnected) BeginAutoLocked();
        }
    }
    if (!enabled && !s_started.load()) { QueuePreferenceWrite(); return true; }
    if (!Start(s_handler.load())) return false;
    return Submit(enabled ? Request::kEnable : Request::kDisable);
}

void SetStandby(bool standby) {
    GetSnapshot();
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_suspended = standby;
        s_boot_pending = false;
        enabled = s_status.enabled;
        CancelInput();
        s_auto_deadline = 0;
        s_status.searching = false;
        if (!standby && enabled && s_status.state != State::kConnected) BeginAutoLocked();
    }
    if (standby) {
        if (s_started.load()) Submit(Request::kSuspend);
    } else if (enabled) {
        Start(s_handler.load());
        Submit(Request::kEnable);
    }
}

void Poll() {
    Request request = Request::kNone;
    bool start_needed = false;
    bool persist_needed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_suspended || s_stopping || s_host_stopped) return;
        const auto now = esp_timer_get_time();
        if (s_boot_pending && now >= s_boot_not_before) {
            s_boot_pending = false;
            if (s_status.enabled && s_status.state != State::kConnected) BeginAutoLocked();
        }
        if (s_status.enabled && s_status.searching && now >= s_auto_deadline) {
            EnableLocked(false);
            s_status.timed_out = true;
            CancelInput();
            request = Request::kDisable;
            ESP_LOGI(TAG, "Automatic connection timed out after 30s; disabled");
        }
        start_needed = !s_started.load() && s_status.enabled && !s_boot_pending;
        persist_needed = s_enabled_dirty && !s_synced.load() && now >= s_persist_retry;
        if (request == Request::kNone && s_synced.load() && s_request == Request::kNone) {
            if (s_status.enabled && s_status.searching && !s_status.scanning &&
                s_status.state != State::kConnected && s_status.state != State::kConnecting && now >= s_next_auto_scan) {
                s_next_auto_scan = now + 1000000;
                request = Request::kAutoScan;
            } else if (s_enabled_dirty && now >= s_persist_retry) request = Request::kPersist;
        }
    }
    if (persist_needed) QueuePreferenceWrite();
    if (start_needed) Start(s_handler.load());
    if (request != Request::kNone) Submit(request, -1, request == Request::kDisable);
}


bool Scan(CommandHandler handler, bool keep_results) {
    if (!Submit(keep_results ? Request::kRefreshScan : Request::kScan, -1, false)) return false;
    if (Start(handler)) return true;
    StopScan();
    return false;
}

void StopScan() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_manual_discovery) return;
    s_manual_discovery = false;
    if (s_request == Request::kScan || s_request == Request::kRefreshScan) s_request = Request::kNone;
    s_stop_scan_requested = true;
    if (s_synced.load() && !s_stopping && !s_host_stopped)
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
}

bool Connect(const char* address) { return s_synced.load() && Submit(Request::kConnect, -1, true, address); }
bool ConnectSaved(const char* address) { return s_synced.load() && Submit(Request::kConnectSaved, -1, true, address); }
bool Forget(const char* address) { return s_synced.load() && Submit(Request::kForget, -1, true, address); }
bool SetLearning(bool enabled) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (enabled && (s_status.state != State::kConnected || s_status.binding_pending || s_status.active_saved < 0 ||
        !SupportsButtonLearning(s_profiles[s_status.active_saved].device.model))) return false;
    if (!enabled && !s_status.learning) return true;
    CancelInput();
    s_status.learning = enabled;
    if (enabled) s_status.operation_failed = false;
    s_status.learned_key = 0;
    ++s_status.revision;
    return true;
}
bool Bind(Action action, uint8_t key) {
    const auto index = static_cast<unsigned>(action);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (index >= 2 || key > 2 || s_status.state != State::kConnected || s_status.active_saved < 0 ||
            !s_status.learning || s_status.binding_pending || s_request != Request::kNone || !s_synced.load()) return false;
        s_requested_address = s_profiles[s_status.active_saved].address;
        s_binding_key = key;
        s_index = index;
        s_request = Request::kBind;
        s_status.binding_pending = true;
        s_status.operation_failed = false;
        ++s_status.revision;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_control);
    }
    return true;
}

void Disconnect() { SetEnabled(false); }

bool PrepareSleep() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_started.load()) return true;
        if (s_stopping || (!s_synced.load() && !s_host_stopped)) return false;
        CancelInput();
        s_stopping = true;
        s_synced = false;
        s_request = Request::kNone;
        s_manual_discovery = false;
        s_stop_scan_requested = false;
    }
    int rc = s_host_stopped ? 0 : nimble_port_stop();
    if (!rc) {
        s_host_stopped = true;
        ble_npl_event_deinit(&s_control);
        rc = nimble_port_deinit();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_stopping = false;
    if (rc) {
        ESP_LOGW(TAG, "BLE sleep preparation failed: %d", rc);
        if (!s_host_stopped) {
            s_synced = ble_hs_synced();
        }
        s_status.state = State::kError;
        ++s_status.revision;
        return false;
    }
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_report_count = 0;
    s_switching = false;
    s_auto_scan = false;
    s_status.state = State::kIdle;
    s_status.scanning = false;
    s_status.active_saved = -1;
    s_status.learning = false;
    s_status.learned_key = 0;
    ++s_status.revision;
    s_control = {};
    s_host_stopped = false;
    s_started = false;
    return true;
}
} // namespace BlePageTurner
