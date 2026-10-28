#include "assets/lang_cellular.h"
#include "settings_test_wifi.h"

#include "settings_test_common.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "settings_common.h"

#include <wifi_scan_lease.h>
#include <wifi_station.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace settings_test_wifi_detail {

constexpr const char* kTag = "SettingsTestWifi";
constexpr const char* kLeaseOwner = "settings_test";
constexpr int kScanTimeoutMs = 15000;
constexpr uint16_t kMaxApRecords = 64;

/**
 * 产测独立扫网会话：
 * - 不读配网/Station 缓存，自己 scan_start + 在 SCAN_DONE handler 内立刻取表
 * - 通过 WifiScanLease 让其它消费者让路（不改它们的连接/配网状态机）
 * - generation 作废离页迟到回调；lease / handler / EventGroup 全路径释放
 */
struct ScanSession {
    SemaphoreHandle_t mu = nullptr;
    StaticSemaphore_t mu_buf{};
    EventGroupHandle_t evt = nullptr;
    esp_event_handler_instance_t handler = nullptr;
    std::vector<wifi_ap_record_t> records;
    bool captured = false;
    bool lease_held = false;
    uint32_t generation = 0;
};

struct SettingsTestScanDoneMsg {
    uint32_t generation = 0;
    bool success = false;
    int count = 0;
};

std::atomic<uint32_t> s_scan_generation{0};
thread_local uint32_t s_worker_generation = 0;

void RebuildWifiListPage();
void ScheduleWifiScan();

void FillUiApsFromRecords(const std::vector<wifi_ap_record_t>& records) {
    SettingsTest_Ui().wifi_aps.clear();
    SettingsTest_Ui().wifi_page = 0;

    std::vector<size_t> order(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&records](size_t a, size_t b) {
        return records[a].rssi > records[b].rssi;
    });

    for (size_t idx : order) {
        char ssid[33];
        std::memcpy(ssid, records[idx].ssid, 32);
        ssid[32] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (const auto& ex : SettingsTest_Ui().wifi_aps) {
            if (ex.ssid == ssid) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        SettingsTestApItem it;
        it.ssid = ssid;
        it.rssi = records[idx].rssi;
        SettingsTest_Ui().wifi_aps.push_back(std::move(it));
    }
}

void OnScanDoneCapture(void* arg, esp_event_base_t base, int32_t id, void* /*data*/) {
    auto* session = static_cast<ScanSession*>(arg);
    if (session == nullptr || base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE) {
        return;
    }
    if (session->mu == nullptr || xSemaphoreTake(session->mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    // 只捕获本会话第一次；避免定时器并发扫的第二次 SCAN_DONE 覆盖
    if (!session->captured) {
        uint16_t ap_num = 0;
        (void)esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > kMaxApRecords) {
            ap_num = kMaxApRecords;
        }
        session->records.clear();
        if (ap_num > 0) {
            session->records.resize(ap_num);
            uint16_t got = ap_num;
            if (esp_wifi_scan_get_ap_records(&got, session->records.data()) == ESP_OK) {
                session->records.resize(got);
            } else {
                session->records.clear();
            }
        }
        session->captured = true;
    }

    if (session->evt != nullptr) {
        xEventGroupSetBits(session->evt, static_cast<EventBits_t>(kSettingsTestBitScanDone));
    }
    xSemaphoreGive(session->mu);
}

// Init and teardown can run on the scan worker and UI task respectively.
// Keep resource ownership and SDK calls in the same lifecycle transaction.
class WifiLifecycleLock {
public:
    WifiLifecycleLock() {
        static StaticSemaphore_t storage;
        static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
        mutex_ = mutex;
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~WifiLifecycleLock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};

bool WifiTeardownLocked() {
    auto& ui = SettingsTest_Ui();
    if (ui.wifi_stack_owned) {
        (void)esp_wifi_stop();
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

bool WifiEnsureForScan() {
    if (!WifiStation::GetInstance().IsUserEnabled()) return false;
    WifiLifecycleLock lock;
    // Teardown may have invalidated this worker while it waited for the lock.
    if (!SettingsTest_IsLive() || s_worker_generation != s_scan_generation.load()) return false;
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode);
    if (mode_err == ESP_OK && mode != WIFI_MODE_NULL) {
        const esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "esp_wifi_start: %s", esp_err_to_name(start_err));
            (void)WifiTeardownLocked();
            return false;
        }
        return true;
    }

    // An initialized NULL-mode driver belongs to another client; do not
    // reinitialize it or destroy that client's default STA on failure.
    if (mode_err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "WiFi unavailable for temporary scan: mode=%d err=%s",
                 static_cast<int>(mode), esp_err_to_name(mode_err));
        return false;
    }
    if (!WifiTeardownLocked()) return false;

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

    if (SettingsTest_Ui().wifi_netif == nullptr) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
            ESP_LOGW(kTag, "default STA already belongs to another client");
            return false;
        }
        SettingsTest_Ui().wifi_netif = esp_netif_create_default_wifi_sta();
    }
    if (SettingsTest_Ui().wifi_netif == nullptr) {
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_init: %s", esp_err_to_name(err));
        (void)WifiTeardownLocked();
        return false;
    }
    SettingsTest_Ui().wifi_stack_owned = true;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        (void)WifiTeardownLocked();
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_wifi_start: %s", esp_err_to_name(err));
        (void)WifiTeardownLocked();
        return false;
    }
    return true;
}

void WifiTeardownIfOwned() {
    WifiLifecycleLock lock;
    esp_wifi_scan_stop();
    (void)WifiTeardownLocked();
}

void CloseWifiPopup() {
    if (SettingsTest_Ui().wifi_mask != nullptr) {
        lv_obj_delete(SettingsTest_Ui().wifi_mask);
    }
    SettingsTest_Ui().wifi_mask = nullptr;
    SettingsTest_Ui().wifi_status = nullptr;
    SettingsTest_Ui().wifi_list = nullptr;
    SettingsTest_Ui().wifi_page_lbl = nullptr;
    SettingsTest_Ui().wifi_rescan_btn = nullptr;
}

void SetWifiRescanEnabled(bool enabled) {
    if (SettingsTest_Ui().wifi_rescan_btn == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(SettingsTest_Ui().wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(SettingsTest_Ui().wifi_rescan_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(SettingsTest_Ui().wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(SettingsTest_Ui().wifi_rescan_btn, LV_OPA_50, 0);
    }
}

int WifiPageCount() {
    if (SettingsTest_Ui().wifi_aps.empty()) {
        return 1;
    }
    return static_cast<int>((SettingsTest_Ui().wifi_aps.size() + kSettingsTestWifiPageSize - 1) /
                            kSettingsTestWifiPageSize);
}

void RebuildWifiListPage() {
    if (!SettingsTest_IsLive() || SettingsTest_Ui().wifi_list == nullptr) {
        return;
    }
    lv_obj_clean(SettingsTest_Ui().wifi_list);

    if (SettingsTest_Ui().wifi_scan_busy) {
        if (SettingsTest_Ui().wifi_status != nullptr) {
            lv_label_set_text(SettingsTest_Ui().wifi_status, Lang::Strings::SETTINGS_TEST_WIFI_SCANNING);
        }
        if (SettingsTest_Ui().wifi_page_lbl != nullptr) {
            lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, "");
        }
        return;
    }

    char status[48];
    std::snprintf(status, sizeof(status), Lang::Strings::SETTINGS_TEST_WIFI_COUNT_FMT,
                  static_cast<int>(SettingsTest_Ui().wifi_aps.size()));
    if (SettingsTest_Ui().wifi_status != nullptr) {
        lv_label_set_text(SettingsTest_Ui().wifi_status, status);
    }

    const int pages = WifiPageCount();
    if (SettingsTest_Ui().wifi_page < 0) {
        SettingsTest_Ui().wifi_page = 0;
    }
    if (SettingsTest_Ui().wifi_page >= pages) {
        SettingsTest_Ui().wifi_page = pages - 1;
    }

    if (SettingsTest_Ui().wifi_page_lbl != nullptr) {
        char foot[32];
        std::snprintf(foot, sizeof(foot), "%d / %d", SettingsTest_Ui().wifi_page + 1, pages);
        lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, foot);
    }

    if (SettingsTest_Ui().wifi_aps.empty()) {
        lv_obj_t* hint = lv_label_create(SettingsTest_Ui().wifi_list);
        lv_label_set_text(hint, Lang::Strings::SETTINGS_TEST_WIFI_NONE);
        lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
        return;
    }

    const int start = SettingsTest_Ui().wifi_page * kSettingsTestWifiPageSize;
    const int end =
        std::min(start + kSettingsTestWifiPageSize, static_cast<int>(SettingsTest_Ui().wifi_aps.size()));
    for (int i = start; i < end; ++i) {
        const auto& ap = SettingsTest_Ui().wifi_aps[static_cast<size_t>(i)];
        lv_obj_t* item = lv_obj_create(SettingsTest_Ui().wifi_list);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, lv_pct(100));
        lv_obj_set_height(item, 40);
        lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, lv_color_black(), 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* ssid = lv_label_create(item);
        lv_label_set_text(ssid, ap.ssid.c_str());
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ssid, fontpack_lv_font_ui(), 0);

        char meta[32];
        std::snprintf(meta, sizeof(meta), "%ddBm", ap.rssi);
        lv_obj_t* ml = lv_label_create(item);
        lv_label_set_text(ml, meta);
        lv_obj_set_style_text_font(ml, fontpack_lv_font_ui(), 0);
    }
}

void OnWifiClose(lv_event_t* /*e*/) {
    CloseWifiPopup();
}

void OnWifiRescan(lv_event_t* /*e*/) {
    ScheduleWifiScan();
}

void OnWifiPrev(lv_event_t* /*e*/) {
    if (SettingsTest_Ui().wifi_page > 0) {
        --SettingsTest_Ui().wifi_page;
        RebuildWifiListPage();
    }
}

void OnWifiNext(lv_event_t* /*e*/) {
    if (SettingsTest_Ui().wifi_page + 1 < WifiPageCount()) {
        ++SettingsTest_Ui().wifi_page;
        RebuildWifiListPage();
    }
}

lv_obj_t* MakePopupBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 88, 44);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, kSettingsBorderW, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void OpenWifiPopup() {
    if (SettingsTest_Ui().wifi_mask != nullptr || !SettingsTest_IsLive() ||
        SettingsTest_Ui().root_scr == nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(SettingsTest_Ui().root_scr);
    SettingsTest_Ui().wifi_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(mask);
    lv_obj_add_event_cb(mask, OnWifiClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 32, kSettingsTestWifiPopupH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_WIFI_NEARBY);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);

    SettingsTest_Ui().wifi_status = lv_label_create(card);
    lv_label_set_text(SettingsTest_Ui().wifi_status, "--");
    lv_obj_set_style_text_font(SettingsTest_Ui().wifi_status, fontpack_lv_font_ui(), 0);
    lv_obj_set_width(SettingsTest_Ui().wifi_status, lv_pct(100));

    SettingsTest_Ui().wifi_list = lv_obj_create(card);
    lv_obj_remove_style_all(SettingsTest_Ui().wifi_list);
    lv_obj_set_width(SettingsTest_Ui().wifi_list, lv_pct(100));
    lv_obj_set_flex_grow(SettingsTest_Ui().wifi_list, 1);
    lv_obj_set_flex_flow(SettingsTest_Ui().wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(SettingsTest_Ui().wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    SettingsTest_Ui().wifi_page_lbl = lv_label_create(card);
    lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, "");
    lv_obj_set_style_text_font(SettingsTest_Ui().wifi_page_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(SettingsTest_Ui().wifi_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(SettingsTest_Ui().wifi_page_lbl, lv_pct(100));

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 48);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    MakePopupBtn(btn_row, Lang::Strings::SETTINGS_TEST_WIFI_PREV, OnWifiPrev);
    SettingsTest_Ui().wifi_rescan_btn = MakePopupBtn(btn_row, Lang::Strings::SETTINGS_TEST_WIFI_RESCAN, OnWifiRescan);
    MakePopupBtn(btn_row, Lang::Strings::SETTINGS_TEST_WIFI_NEXT, OnWifiNext);
    MakePopupBtn(btn_row, Lang::Strings::SETTINGS_TEST_WIFI_CLOSE, OnWifiClose);

    SetWifiRescanEnabled(!SettingsTest_Ui().wifi_scan_busy);
    RebuildWifiListPage();
}

void OnWifiRowClicked(lv_event_t* /*e*/) {
    OpenWifiPopup();
}

void AsyncWifiScanDone(void* p) {
    auto* msg = static_cast<SettingsTestScanDoneMsg*>(p);
    if (msg->generation != s_scan_generation.load()) { delete msg; return; }
    // Current session owns the scan result.
    SettingsTest_Ui().wifi_scan_busy = false;
    if (!SettingsTest_IsLive()) {
        delete msg;
        return;
    }
    if (msg->success && msg->count > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_TEST_WIFI_FOUND_FMT, msg->count);
        SettingsTest_SetRowValue(SettingsTest_Ui().wifi, buf);
        SettingsTest_SetRowStatus(SettingsTest_Ui().wifi, true);
    } else if (msg->success) {
        SettingsTest_SetRowValue(SettingsTest_Ui().wifi, Lang::Strings::SETTINGS_TEST_WIFI_NONE);
        SettingsTest_SetRowStatus(SettingsTest_Ui().wifi, false);
    } else {
        SettingsTest_SetRowValue(SettingsTest_Ui().wifi, Lang::Strings::SETTINGS_TEST_WIFI_SCAN_FAIL);
        SettingsTest_SetRowStatus(SettingsTest_Ui().wifi, false);
    }
    SetWifiRescanEnabled(true);
    RebuildWifiListPage();
    delete msg;
}

void PostWifiScanDone(bool success, int count) {
    auto* msg = new (std::nothrow) SettingsTestScanDoneMsg{s_worker_generation, success, count};
    if (msg == nullptr) {
        SettingsTest_Ui().wifi_scan_busy = false;
        return;
    }
    if (!ScreenLvAsyncUrgent(AsyncWifiScanDone, msg)) {
        SettingsTest_Ui().wifi_scan_busy = false;
        delete msg;
    }
}

void DestroyScanSession(ScanSession* session) {
    if (session == nullptr) {
        return;
    }
    if (session->handler != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, session->handler);
        session->handler = nullptr;
    }
    if (session->evt != nullptr) {
        vEventGroupDelete(session->evt);
        session->evt = nullptr;
    }
    if (session->lease_held) {
        WifiScanLease::Release();
        session->lease_held = false;
    }
    session->records.clear();
    // StaticSemaphore 随 session 释放；勿 vSemaphoreDelete
}

void WifiScanTask(void* /*arg*/) {
    const uint32_t my_gen = s_scan_generation;
    s_worker_generation = my_gen;
    SettingsTest_Ui().wifi_scan_busy = true;

    // 任务末尾 vTaskDelete 不跑 C++ 析构，会话必须堆分配并在删任务前 delete
    ScanSession* session = new (std::nothrow) ScanSession();
    if (session == nullptr) {
        PostWifiScanDone(false, 0);
        vTaskDelete(nullptr);
        return;
    }
    session->generation = my_gen;
    session->mu = xSemaphoreCreateMutexStatic(&session->mu_buf);
    session->evt = xEventGroupCreate();

    auto finish_fail = [&]() {
        DestroyScanSession(session);
        delete session;
        session = nullptr;
        if (my_gen == s_scan_generation) {
            PostWifiScanDone(false, 0);
        } else {
            SettingsTest_Ui().wifi_scan_busy = false;
        }
        vTaskDelete(nullptr);
    };

    if (session->mu == nullptr || session->evt == nullptr) {
        ESP_LOGE(kTag, "session alloc failed");
        finish_fail();
        return;
    }

    if (!WifiScanLease::TryAcquire(kLeaseOwner)) {
        ESP_LOGW(kTag, "lease busy");
        finish_fail();
        return;
    }
    session->lease_held = true;

    if (esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &OnScanDoneCapture,
                                            session, &session->handler) != ESP_OK) {
        ESP_LOGE(kTag, "register SCAN_DONE failed");
        finish_fail();
        return;
    }

    if (!SettingsTest_IsLive() || my_gen != s_scan_generation) {
        DestroyScanSession(session);
        delete session;
        SettingsTest_Ui().wifi_scan_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    if (!WifiEnsureForScan()) {
        finish_fail();
        return;
    }

    (void)esp_wifi_scan_stop();
    xEventGroupClearBits(session->evt, static_cast<EventBits_t>(kSettingsTestBitScanDone));
    if (xSemaphoreTake(session->mu, pdMS_TO_TICKS(200)) == pdTRUE) {
        session->captured = false;
        session->records.clear();
        xSemaphoreGive(session->mu);
    }

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    const esp_err_t start_err = esp_wifi_scan_start(&cfg, false);
    if (start_err != ESP_OK) {
        ESP_LOGW(kTag, "scan_start: %s", esp_err_to_name(start_err));
        finish_fail();
        return;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        session->evt, static_cast<EventBits_t>(kSettingsTestBitScanDone), pdTRUE, pdTRUE,
        pdMS_TO_TICKS(kScanTimeoutMs));

    std::vector<wifi_ap_record_t> local_records;
    bool ok = false;
    if ((bits & kSettingsTestBitScanDone) &&
        xSemaphoreTake(session->mu, pdMS_TO_TICKS(200)) == pdTRUE) {
        ok = session->captured;
        local_records = std::move(session->records);
        xSemaphoreGive(session->mu);
    } else {
        (void)esp_wifi_scan_stop();
        ESP_LOGW(kTag, "scan timeout or capture missed");
    }

    DestroyScanSession(session);
    delete session;
    session = nullptr;

    if (my_gen != s_scan_generation || !SettingsTest_IsLive()) {
        local_records.clear();
        SettingsTest_Ui().wifi_scan_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    if (!ok) {
        local_records.clear();
        PostWifiScanDone(false, 0);
        vTaskDelete(nullptr);
        return;
    }

    FillUiApsFromRecords(local_records);
    local_records.clear();
    PostWifiScanDone(true, static_cast<int>(SettingsTest_Ui().wifi_aps.size()));
    vTaskDelete(nullptr);
}

void ScheduleWifiScan() {
    if (!SettingsTest_IsLive() || SettingsTest_Ui().wifi_scan_busy) {
        return;
    }
    ++s_scan_generation;
    SettingsTest_SetRowValue(SettingsTest_Ui().wifi, Lang::Strings::SETTINGS_TEST_SCANNING);
    SetWifiRescanEnabled(false);
    RebuildWifiListPage();
    SettingsTest_Ui().wifi_scan_busy = true;
    if (xTaskCreate(WifiScanTask, "set_wifi_scan", 6144, nullptr, 5, nullptr) != pdPASS) {
        SettingsTest_Ui().wifi_scan_busy = false;
        SettingsTest_SetRowValue(SettingsTest_Ui().wifi, Lang::Strings::CLOUD_TASK_FAIL);
        SettingsTest_SetRowStatus(SettingsTest_Ui().wifi, false);
        SetWifiRescanEnabled(true);
    }
}

void InvalidateInFlightScan() {
    ++s_scan_generation;
    (void)esp_wifi_scan_stop();
    // 进行中的任务在 WaitBits 返回后见 generation 变化会自行 Release/清 busy
}

}  // namespace settings_test_wifi_detail

void SettingsTestWifi_ScheduleScan() {
    settings_test_wifi_detail::ScheduleWifiScan();
}

void SettingsTestWifi_Teardown() {
    settings_test_wifi_detail::InvalidateInFlightScan();
    settings_test_wifi_detail::WifiTeardownIfOwned();
}

void SettingsTestWifi_OnRowClicked(lv_event_t* e) {
    settings_test_wifi_detail::OnWifiRowClicked(e);
}
