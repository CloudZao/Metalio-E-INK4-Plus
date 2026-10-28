#include "assets/lang_screen.h"
#include "assets/lang_network.h"
#pragma GCC optimize("O1")

// network_wifi.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_wifi.h"
#include "network_screen_ui.h"

#include "cloud_screen/ap/ap_qr.h"
#include "book_screen/book_text_util.h"
#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "ssid_manager.h"
#include "vk_key_handler.h"
#include "wifi_station.h"

#include <algorithm>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
constexpr uint32_t kNetworkTaskCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kNetworkTaskCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif

static std::mutex s_network_work;

static const char* Network_WifiUnavailableReason() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual != nullptr && dual->GetNetworkType() == NetworkType::CELLULAR) {
        return Lang::Strings::CLOUD_CELLULAR_NEED_WIFI;
    }
    return WifiStation::GetInstance().IsUserEnabled() ? nullptr : Lang::Strings::WIFI_DISABLED_RETRY;
}

void Network_StopForScreen() {
    esp_wifi_scan_stop();
    if (Network_State().evt_group) {
        xEventGroupSetBits(Network_State().evt_group, kBitScanDone | kBitDisconnected);
    }
    std::lock_guard<std::mutex> lock(s_network_work);
    if (Network_State().transfer_mode) {
        ApTransferRequestStop();
        ApTransferStop();
    }
    Network_WifiTeardownForScreen();
}

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

static void Network_DestroyOwnedNetif() {
    // esp_wifi_init 失败时也可能已 create_default_wifi_sta；须用 default 析构卸驱动挂接
    if (Network_State().netif != nullptr) {
        esp_netif_destroy_default_wifi(Network_State().netif);
        Network_State().netif = nullptr;
    }
}

bool Network_WifiInitForScreen() {
    if (Network_WifiUnavailableReason() != nullptr) return false;
    if (Network_State().wifi_initialized) {
        return true;
    }

    wifi_mode_t mode_before = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode_before);
    Network_State().wifi_station_was_active = (mode_err == ESP_OK && mode_before != WIFI_MODE_NULL);
    if (Network_State().wifi_station_was_active) {
        // 驱动已初始化时先释放旧 STA；暂停状态无需恢复后再关闭。
        auto& sta = WifiStation::GetInstance();
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

    if (Network_State().netif == nullptr) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) {
            ESP_LOGW(TAG, "default STA already belongs to another client");
            return false;
        }
        Network_State().netif = esp_netif_create_default_wifi_sta();
    }
    if (Network_State().netif == nullptr) {
        ESP_LOGE(TAG, "create sta netif failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        Network_DestroyOwnedNetif();
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Network_WifiEvtHandler, nullptr,
                                              &Network_State().wifi_evt_inst);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Network_WifiEvtHandler, nullptr,
                                              &Network_State().ip_evt_inst);

    // 驱动已 init：后续失败走完整 Teardown（含 deinit），勿只毁 netif
    Network_State().wifi_initialized = true;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        Network_WifiTeardownForScreen();
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        Network_WifiTeardownForScreen();
        return false;
    }
    ESP_LOGI(TAG, "local STA stack ready");
    return true;
}

void Network_WifiTeardownForScreen() {
    // wifi_initialized=false 时仍可能残留 netif（init 中途失败）
    if (!Network_State().wifi_initialized) {
        Network_DestroyOwnedNetif();
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
    Network_DestroyOwnedNetif();
    Network_State().wifi_initialized = false;
    if (Network_State().wifi_station_was_active) {
        WifiStation::GetInstance().Start();
    }
    Network_State().wifi_station_was_active = false;
    ESP_LOGI(TAG, "local STA stack torn down");
}

static void ScanOnce() {
    if (const char* reason = Network_WifiUnavailableReason()) {
        if (reason == Lang::Strings::WIFI_DISABLED_RETRY) {
            auto notify = [](void*) { ScreenShowToast(Lang::Strings::WIFI_DISABLED_RETRY); };
            ScreenLvAsync(notify);
        }
        Network_PostScanResult(reason);
        Network_PostScanBtnEnabled(true);
        return;
    }
    Network_State().scan_in_progress = true;
    Network_PostScanBtnEnabled(false);
    Network_PostStatus(Lang::Strings::NETWORK_SCANNING);
    Network_PostRebuildList();

    if (!Network_State().screen_active) return;
    if (!Network_State().wifi_initialized) {
        Network_PostStatus(Lang::Strings::NETWORK_WIFI_INIT);
        if (!Network_WifiInitForScreen()) {
            const char* reason = Network_WifiUnavailableReason();
            Network_PostScanResult(reason != nullptr ? reason : Lang::Strings::NETWORK_WIFI_INIT_FAIL);
            Network_State().scan_in_progress = false;
            Network_PostRebuildList();
            Network_PostScanBtnEnabled(true);

            return;
        }
    }

    xEventGroupClearBits(Network_State().evt_group, kBitScanDone);
    if (!Network_State().screen_active) return;
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        Network_PostScanResult(Lang::Strings::NETWORK_SCAN_FAIL);
        Network_State().scan_in_progress = false;
        Network_PostRebuildList();
        Network_PostScanBtnEnabled(true);

        return;
    }

    const EventBits_t bits =
        xEventGroupWaitBits(Network_State().evt_group, kBitScanDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(15000));
    if (!Network_State().screen_active) return;
    if (!(bits & kBitScanDone)) {
        Network_PostScanResult(Lang::Strings::NETWORK_SCAN_TIMEOUT);
        Network_State().scan_in_progress = false;
        Network_PostRebuildList();
        Network_PostScanBtnEnabled(true);

        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    ap_num = std::min<uint16_t>(ap_num, 64);
    std::unique_ptr<wifi_ap_record_t, decltype(&heap_caps_free)> records(nullptr, heap_caps_free);
    if (ap_num > 0) {
        records.reset(static_cast<wifi_ap_record_t*>(heap_caps_malloc(ap_num * sizeof(wifi_ap_record_t), kNetworkTaskCaps)));
        if (!records || esp_wifi_scan_get_ap_records(&ap_num, records.get()) != ESP_OK) {
            esp_wifi_clear_ap_list();
            Network_PostScanResult(Lang::Strings::NETWORK_SCAN_FAIL);
            Network_State().scan_in_progress = false;
            Network_PostRebuildList();
            Network_PostScanBtnEnabled(true);
            return;
        }
        std::sort(records.get(), records.get() + ap_num,
                  [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    }

    Network_State().scan_results.clear();
    for (uint16_t i = 0; i < ap_num; ++i) {
        const auto& r = records.get()[i];
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
    Network_PostScanResult(buf);
    Network_PostRebuildList();
    Network_PostScanBtnEnabled(true);
}

void Network_ScanTask(void*) {
    {
        std::lock_guard<std::mutex> lock(s_network_work);
        if (Network_State().screen_active) ScanOnce();
        Network_State().scan_in_progress = false;
    }
    vTaskDeleteWithCaps(nullptr);
}

void Network_ScheduleScan() {
    if (const char* reason = Network_WifiUnavailableReason()) {
        if (reason == Lang::Strings::WIFI_DISABLED_RETRY) ScreenShowToast(reason);
        Network_PostScanResult(reason);
        return;
    }
    if (Network_State().scan_in_progress) {
        return;
    }
    if (Network_State().connect_in_progress) {
        Network_PostStatus(Lang::Strings::NETWORK_BUSY_CONNECT);
        return;
    }
    if (Network_State().transfer_mode) {
        if (Network_State().transfer_scan_timer) {
            lv_timer_delete(Network_State().transfer_scan_timer);
            Network_State().transfer_scan_timer = nullptr;
        }
        Network_State().transfer_scan_pending = true;
        Network_State().transfer_scan_message.clear();
    }
    Network_State().scan_in_progress = true;
    if (xTaskCreateWithCaps(Network_ScanTask, "net_scan", 4096, nullptr, 5, nullptr, kNetworkTaskCaps) != pdPASS) {
        Network_State().scan_in_progress = false;
        Network_PostScanResult(Lang::Strings::NETWORK_SCAN_TASK_FAIL);
    }
}

void Network_AsyncShowFailure(void* p) {
    auto* msg = static_cast<AsyncFailMsg*>(p);
    if (Network_ScreenAlive() && msg->generation == Network_State().generation.load()) {
        Network_State().retry_ssid = Network_State().pending_ssid;
        char buf[192];
        snprintf(buf, sizeof(buf), "%s\n%s", msg->title.c_str(), msg->detail.c_str());
        Network_OpenStatusOverlay(buf);
        Network_State().fail_close_timer = lv_timer_create(
            [](lv_timer_t* timer) {
                Network_State().fail_close_timer = nullptr;
                if (Network_ScreenAlive()) {
                    Network_CloseStatusOverlay();
                }
                lv_timer_delete(timer);
            },
            2500, nullptr);
        lv_timer_set_repeat_count(Network_State().fail_close_timer, 1);
    }
    delete msg;
}

void Network_PostShowFailure(const std::string& title, const std::string& detail) {
    if (!Network_State().screen_active) return;
    auto* msg = new AsyncFailMsg{Network_State().generation.load(), title, detail};
    if (!ScreenLvAsync(Network_AsyncShowFailure, msg)) {
        delete msg;
    }
}

void Network_AsyncOpenSuccess(void* p) {
    auto* msg = static_cast<AsyncConnectionMsg*>(p);
    if (Network_ScreenAlive() && msg->generation == Network_State().generation.load()) {
        Network_State().retry_ssid.clear();
        Network_ClosePasswordPage();
        if (Network_State().transfer_mode) {
            Network_OpenStatusOverlay("WIFI传输已就绪");
            auto* card = lv_obj_get_parent(Network_State().ui.status_msg);
            lv_obj_set_style_pad_row(card, UiSy(8), 0);
            if (const auto* font = fontpack_lv_font_get(30, 4)) {
                lv_obj_set_style_text_font(Network_State().ui.status_msg, font, 0);
            }
            auto add_text = [card](const char* text) {
                auto* label = lv_label_create(card);
                lv_obj_set_width(label, LV_PCT(100));
                lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_font(label, Network_UiFont(), 0);
                lv_obj_set_style_text_color(label, lv_color_black(), 0);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_text(label, text);
                return label;
            };
            auto* network = add_text("");
            lv_obj_set_height(network, Network_UiLineH());
            add_text("手机或电脑连接同一WIFI\n扫码或用浏览器打开下方网址");
            auto* qr = ApQrCreate(card);
            ApQrPaint(qr, Network_State().transfer_info.url);
            add_text(Network_State().transfer_info.url);
            add_text("切换传输方式或退出传输页将停止传输");
            Network_MakeBtn(card, "返回列表", kPwdBtnW, kPwdBtnH,
                [](lv_event_t*) { Network_CloseStatusOverlay(); });
            char network_name[80];
            snprintf(network_name, sizeof(network_name), "当前网络：%s", msg->ssid.c_str());
            Book_SetFilenameMiddle(network, network_name);
        } else {
            char headline[96];
            snprintf(headline, sizeof(headline), Lang::Strings::NETWORK_CONNECTED_FMT, msg->ssid.c_str());
            Network_OpenRestartCountdown(headline);
        }
    }
    delete msg;
}

void Network_PostOpenSuccess(const std::string& ssid) {
    if (!Network_State().screen_active) return;
    auto* msg = new AsyncConnectionMsg{ssid, Network_State().generation.load()};
    if (!ScreenLvAsync(Network_AsyncOpenSuccess, msg)) {
        delete msg;
    }
}

static void ConnectOnce(void* arg) {
    auto* ctx = static_cast<ConnectCtx*>(arg);
    char buf[128];
    snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTING_FMT, ctx->ssid.c_str());
    Network_PostStatus(buf);

    if (Network_State().transfer_mode) {
        ApTransferStop();
        Network_State().transfer_info = {};
    }
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wc = {};
    memcpy(wc.sta.ssid, ctx->ssid.data(), ctx->ssid.size());
    memcpy(wc.sta.password, ctx->password.data(), ctx->password.size());
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.failure_retry_cnt = 1;

    xEventGroupClearBits(Network_State().evt_group, kBitConnected | kBitDisconnected);
    Network_State().last_disconnect_reason = 0;
    if (!Network_State().screen_active) {
        delete ctx;
        return;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL, Lang::Strings::NETWORK_CONNECT_FAIL_REFRESH);
        Network_State().connect_in_progress = false;
        delete ctx;
        return;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL, Lang::Strings::NETWORK_CONNECT_FAIL_REFRESH);
        Network_State().connect_in_progress = false;
        delete ctx;
        return;
    }

    const EventBits_t bits = xEventGroupWaitBits(Network_State().evt_group, kBitConnected | kBitDisconnected, pdTRUE,
                                                 pdFALSE, pdMS_TO_TICKS(15000));
    if (!Network_State().screen_active) {
        delete ctx;
        return;
    }
    if (bits & kBitConnected) {
        SsidManager::GetInstance().AddSsid(ctx->ssid, ctx->password);
        snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTED_FMT, ctx->ssid.c_str());
        Network_PostStatus(buf);
        Network_PostRebuildList();
        if (Network_State().transfer_mode && !WifiTransferStart(&Network_State().transfer_info)) {
            Network_PostShowFailure(Lang::Strings::NETWORK_CONNECT_FAIL,
                Network_State().transfer_info.fail ? Network_State().transfer_info.fail : Lang::Strings::CLOUD_AP_FAIL_HTTP);
        } else {
            Network_PostOpenSuccess(ctx->ssid);
        }
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
}

void Network_ConnectTask(void* arg) {
    {
        std::lock_guard<std::mutex> lock(s_network_work);
        if (Network_State().screen_active) ConnectOnce(arg);
        else delete static_cast<ConnectCtx*>(arg);
        Network_State().connect_in_progress = false;
    }
    vTaskDeleteWithCaps(nullptr);
}

void Network_ScheduleConnect(const std::string& ssid, const std::string& password) {
    if (const char* reason = Network_WifiUnavailableReason()) {
        if (reason == Lang::Strings::WIFI_DISABLED_RETRY) ScreenShowToast(reason);
        Network_PostStatus(reason);
        return;
    }
    if (Network_State().scan_in_progress || Network_State().connect_in_progress) {
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
        const char* reason = Network_WifiUnavailableReason();
        Network_PostStatus(reason != nullptr ? reason : Lang::Strings::NETWORK_WIFI_INIT_FAIL);
        return;
    }

    Network_State().pending_ssid = ssid;
    auto* ctx = new ConnectCtx{ssid, password};
    Network_State().connect_in_progress = true;
    char buf[96];
    snprintf(buf, sizeof(buf), Lang::Strings::NETWORK_CONNECTING_FMT, ssid.c_str());
    Network_OpenStatusOverlay(buf);

    // 连接成功后写入 NVS，Flash 缓存关闭期间必须使用内部 RAM 栈。
    if (xTaskCreateWithCaps(Network_ConnectTask, "net_conn", 4096, ctx, 5, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        delete ctx;
        Network_State().connect_in_progress = false;
        Network_CloseStatusOverlay();
        Network_PostStatus(Lang::Strings::NETWORK_CONNECT_TASK_FAIL);
    }
}

