// network_wifi.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_wifi.h"
#include "network_screen/network_screen_priv.h"

#include "application.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "ssid_manager.h"
#include "vk_key_handler.h"
#include "wifi_station.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

void Network_WifiEvtHandler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
    if (Network_State().evt_group == nullptr) {
        return;
    }
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            xEventGroupSetBits(Network_State().evt_group, kBitScanDone);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* evt = static_cast<wifi_event_sta_disconnected_t*>(data);
            Network_State().last_disconnect_reason = (evt != nullptr) ? evt->reason : 0;
            xEventGroupSetBits(Network_State().evt_group, kBitDisconnected);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(Network_State().evt_group, kBitConnected);
    }
}

void Network_AcquireUiKeepNet() {
    if (Network_State().ui_net_held) {
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    Network_State().ui_net_held = true;
}

void Network_ReleaseUiKeepNet() {
    if (!Network_State().ui_net_held) {
        return;
    }
    PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    Network_State().ui_net_held = false;
}

bool Network_WifiInitForScreen() {
    if (Network_State().wifi_initialized) {
        return true;
    }

    wifi_mode_t mode_before = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode_before);
    Network_State().wifi_station_was_active = (mode_err == ESP_OK && mode_before != WIFI_MODE_NULL);
    if (Network_State().wifi_station_was_active) {
        // LpPaused 时栈仍 init，Stop 会 deinit；Resume 路径也走 Stop 更干净
        auto& sta = WifiStation::GetInstance();
        if (sta.IsLpPaused()) {
            sta.ResumeFromLp();
        }
        sta.Stop();
    }

    if (Network_State().evt_group == nullptr) {
        Network_State().evt_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(Network_State().evt_group, 0xFFFFFF);
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop: %s", esp_err_to_name(err));
        return false;
    }

    Network_State().netif = esp_netif_create_default_wifi_sta();
    if (Network_State().netif == nullptr) {
        ESP_LOGE(TAG, "create sta netif failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Network_WifiEvtHandler, nullptr,
                                              &Network_State().wifi_evt_inst);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Network_WifiEvtHandler, nullptr,
                                              &Network_State().ip_evt_inst);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    Network_State().wifi_initialized = true;
    ESP_LOGI(TAG, "local STA stack ready");
    return true;
}

void Network_WifiTeardownForScreen() {
    if (!Network_State().wifi_initialized) {
        return;
    }
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    if (Network_State().wifi_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, Network_State().wifi_evt_inst);
        Network_State().wifi_evt_inst = nullptr;
    }
    if (Network_State().ip_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, Network_State().ip_evt_inst);
        Network_State().ip_evt_inst = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (Network_State().netif != nullptr) {
        esp_netif_destroy(Network_State().netif);
        Network_State().netif = nullptr;
    }
    Network_State().wifi_initialized = false;
    if (Network_State().wifi_station_was_active) {
        WifiStation::GetInstance().Start();
    }
    Network_State().wifi_station_was_active = false;
    ESP_LOGI(TAG, "local STA stack torn down");
}

void Network_ScanTask(void* /*arg*/) {
    Network_State().scan_in_progress = true;
    Network_PostScanBtnEnabled(false);
    Network_PostStatus(Lang::Strings::NETWORK_SCANNING);
    Network_PostRebuildList();

    if (!Network_State().wifi_initialized) {
        Network_PostStatus(Lang::Strings::NETWORK_WIFI_INIT);
        if (!Network_WifiInitForScreen()) {
            Network_PostStatus(Lang::Strings::NETWORK_WIFI_INIT_FAIL);
            Network_State().scan_in_progress = false;
            Network_PostRebuildList();
            Network_PostScanBtnEnabled(true);
            vTaskDelete(nullptr);
            return;
        }
    }

    xEventGroupClearBits(Network_State().evt_group, kBitScanDone);
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        Network_PostStatus(Lang::Strings::NETWORK_SCAN_FAIL);
        Network_State().scan_in_progress = false;
        Network_PostRebuildList();
        Network_PostScanBtnEnabled(true);
        vTaskDelete(nullptr);
        return;
    }

    const EventBits_t bits =
        xEventGroupWaitBits(Network_State().evt_group, kBitScanDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(15000));
    if (!(bits & kBitScanDone)) {
        Network_PostStatus(Lang::Strings::NETWORK_SCAN_TIMEOUT);
        Network_State().scan_in_progress = false;
        Network_PostRebuildList();
        Network_PostScanBtnEnabled(true);
        vTaskDelete(nullptr);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    std::vector<wifi_ap_record_t> records;
    if (ap_num > 0) {
        if (ap_num > 64) {
            ap_num = 64;
        }
        records.resize(ap_num);
        uint16_t got = ap_num;
        if (esp_wifi_scan_get_ap_records(&got, records.data()) == ESP_OK) {
            records.resize(got);
        } else {
            records.clear();
        }
    }

    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });

    Network_State().scan_results.clear();
    for (const auto& r : records) {
        char ssid[33];
        std::memcpy(ssid, r.ssid, 32);
        ssid[32] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (const auto& ex : Network_State().scan_results) {
            if (ex.ssid == ssid) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        ApItem it;
        it.ssid = ssid;
        it.rssi = r.rssi;
        it.authmode = r.authmode;
        Network_State().scan_results.push_back(std::move(it));
    }

    // 先清 in-progress，再投递 UI：避免排队中的 Rebuild 仍看到扫描中而盖掉结果列表
    Network_State().nearby_page = 0;
    Network_State().scan_in_progress = false;

    char buf[64];
    snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_SCAN_DONE_FMT,
             static_cast<int>(Network_State().scan_results.size()));
    Network_PostStatus(buf);
    Network_PostRebuildList();
    Network_PostScanBtnEnabled(true);
    vTaskDelete(nullptr);
}

void Network_ScheduleScan() {
    if (Network_State().scan_in_progress) {
        return;
    }
    if (Network_State().connect_in_progress) {
        Network_PostStatus(Lang::Strings::NETWORK_BUSY_CONNECT);
        return;
    }
    if (xTaskCreate(Network_ScanTask, "net_scan", 4096, nullptr, 5, nullptr) != pdPASS) {
        Network_PostStatus(Lang::Strings::NETWORK_SCAN_TASK_FAIL);
    }
}

void Network_AsyncShowFailure(void* p) {
    auto* msg = static_cast<AsyncFailMsg*>(p);
    if (Network_ScreenAlive()) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s\n%s", msg->title.c_str(), msg->detail.c_str());
        Network_OpenStatusOverlay(buf);
        if (Network_State().fail_close_timer != nullptr) {
            lv_timer_delete(Network_State().fail_close_timer);
            Network_State().fail_close_timer = nullptr;
        }
        Network_State().fail_close_timer = lv_timer_create(
            [](lv_timer_t* timer) {
                if (Network_ScreenAlive()) {
                    Network_CloseStatusOverlay();
                }
                Network_State().fail_close_timer = nullptr;
                lv_timer_delete(timer);
            },
            2500, nullptr);
        lv_timer_set_repeat_count(Network_State().fail_close_timer, 1);
    }
    delete msg;
}

void Network_PostShowFailure(const std::string& title, const std::string& detail) {
    auto* msg = new AsyncFailMsg{title, detail};
    if (lv_async_call(Network_AsyncShowFailure, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

void Network_AsyncOpenSuccess(void* p) {
    auto* ssid = static_cast<std::string*>(p);
    if (Network_ScreenAlive()) {
        Network_ClosePasswordPage();
        char headline[96];
        snprintf(headline, sizeof(headline), Lang::Strings::NETWORK_CONNECTED_FMT, ssid->c_str());
        Network_OpenRestartCountdown(headline);
    }
    delete ssid;
}

void Network_PostOpenSuccess(const std::string& ssid) {
    auto* msg = new std::string(ssid);
    if (lv_async_call(Network_AsyncOpenSuccess, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

void Network_ConnectTask(void* arg) {
    auto* ctx = static_cast<ConnectCtx*>(arg);
    char buf[128];
    snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTING_FMT, ctx->ssid.c_str());
    Network_PostStatus(buf);

    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wc = {};
    strlcpy(reinterpret_cast<char*>(wc.sta.ssid), ctx->ssid.c_str(), 32);
    strlcpy(reinterpret_cast<char*>(wc.sta.password), ctx->password.c_str(), 64);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.failure_retry_cnt = 1;

    xEventGroupClearBits(Network_State().evt_group, kBitConnected | kBitDisconnected);
    Network_State().last_disconnect_reason = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL, Lang::Strings::NETWORK_CONNECT_FAIL_REFRESH);
        Network_State().connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL, Lang::Strings::NETWORK_CONNECT_FAIL_REFRESH);
        Network_State().connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    const EventBits_t bits = xEventGroupWaitBits(Network_State().evt_group, kBitConnected | kBitDisconnected, pdTRUE,
                                                 pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & kBitConnected) {
        SsidManager::GetInstance().AddSsid(ctx->ssid, ctx->password);
        snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTED_FMT, ctx->ssid.c_str());
        Network_PostStatus(buf);
        Network_PostRebuildList();
        Network_PostOpenSuccess(ctx->ssid);
    } else if (bits & kBitDisconnected) {
        const char* mapped = Network_DisconnectReasonText(Network_State().last_disconnect_reason);
        std::string detail;
        if (mapped != nullptr) {
            detail = mapped;
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), Lang::Strings::NETWORK_ERR_REASON_FMT, Network_State().last_disconnect_reason);
            detail = tmp;
        }
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL, detail);
    } else {
        esp_wifi_disconnect();
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_TIMEOUT, Lang::Strings::NETWORK_CONNECT_TIMEOUT_HINT);
    }

    Network_State().connect_in_progress = false;
    delete ctx;
    vTaskDelete(nullptr);
}

void Network_ScheduleConnect(const std::string& ssid, const std::string& password) {
    if (Network_State().connect_in_progress) {
        return;
    }
    if (ssid.empty() || ssid.size() > kMaxSsidLen) {
        Network_PostStatus(Lang::Strings::NETWORK_SSID_INVALID);
        return;
    }
    if (password.size() > kMaxPasswordLen) {
        Network_PostStatus(Lang::Strings::NETWORK_PWD_TOO_LONG);
        return;
    }
    if (!Network_State().wifi_initialized && !Network_WifiInitForScreen()) {
        Network_PostStatus(Lang::Strings::NETWORK_WIFI_INIT_FAIL);
        return;
    }

    auto* ctx = new ConnectCtx{ssid, password};
    Network_State().connect_in_progress = true;
    char buf[96];
    snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTING_FMT, ssid.c_str());
    Network_OpenStatusOverlay(buf);

    if (xTaskCreate(Network_ConnectTask, "net_conn", 4096, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        Network_State().connect_in_progress = false;
        Network_CloseStatusOverlay();
        Network_PostStatus(Lang::Strings::NETWORK_CONNECT_TASK_FAIL);
    }
}

