#include "pocket_wifi_connection.h"
#include "pocket_wifi_connection_model.h"
#include "pocket_wifi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <utility>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace pocket {
ESP_EVENT_DEFINE_BASE(PLUS_WIFI_CONTROL);
namespace {
constexpr char kTag[] = "PocketWifi";
constexpr int64_t kConnectTimeoutUs = 12000000;
constexpr int64_t kScanTimeoutUs = 10000000;
enum class Kind { Scan, Connect, Disconnect };
struct Request { Kind kind; uint32_t generation; char ssid[33]{}; };

std::mutex view_mutex;
std::mutex request_mutex;
WifiConnectionView view;
std::atomic<QueueHandle_t> queue{nullptr};
std::atomic<bool> starting{false};
std::atomic<bool> suspend_requested{false};
std::atomic<bool> suspended{false};
std::atomic<bool> scan_pending{false};
std::atomic<uint32_t> generation{0};
std::atomic<bool> associated{false};
std::atomic<bool> fresh_ip{false};
std::atomic<bool> scan_done{false};
std::atomic<bool> radio_started{false};
std::atomic<bool> stop_observed{false};
std::mutex link_mutex;
std::string active_ssid;
std::array<uint8_t, 6> active_bssid{};
bool bssid_known = false;
std::atomic<bool> accept_link_events{false};
std::atomic<uint32_t> link_generation{0};
std::atomic<uint32_t> barrier_next{0}, barrier_done{0};
std::atomic<int> disconnect_reason{0};
esp_netif_t* netif = nullptr;  // Created once; never destroyed while callbacks exist.
esp_event_handler_instance_t wifi_handler = nullptr;
esp_event_handler_instance_t ip_handler = nullptr;
esp_event_handler_instance_t barrier_handler = nullptr;
bool AcceptLinkEvents() {
    return wifi_detail::AcceptLinkEvent(accept_link_events.load(), link_generation.load(),
                                       generation.load(), suspend_requested.load());
}

bool Equal(const WifiConnectionView& a, const WifiConnectionView& b) {
    return a.ready == b.ready && a.startup_complete == b.startup_complete &&
        a.connection_id == b.connection_id && a.scanning == b.scanning && a.connecting == b.connecting &&
        a.connected == b.connected && a.signal_bars == b.signal_bars && a.ssid == b.ssid &&
        a.ip == b.ip && a.message == b.message && a.failure == b.failure &&
        a.reason_code == b.reason_code && a.access_points.size() == b.access_points.size() &&
        std::equal(a.access_points.begin(), a.access_points.end(), b.access_points.begin(),
            [](const WifiAccessPoint& x, const WifiAccessPoint& y) {
                return x.ssid == y.ssid && x.rssi == y.rssi && x.secured == y.secured;
            });
}

template<class Update> void Publish(Update update) {
    std::lock_guard<std::mutex> lock(view_mutex);
    const auto previous = view;
    update(view);
    if (!Equal(previous, view)) ++view.revision;
}

void ClearLink() {
    associated.store(false);
    fresh_ip.store(false);
    Publish([](auto& v) {
        const bool was_connected = v.connected;
        v.connected = false; v.signal_bars = 0; v.ip.clear();
        if (was_connected && !v.connecting && !v.scanning)
            v.message = "Wi-Fi 已断开，正在等待重新连接";
    });
}

WifiFailure ConnectionFailure(int reason, bool was_associated) {
    if (reason == 2 || reason == 15 || reason == 16 || reason == 202 || reason == 204)
        return WifiFailure::Authentication;
    if (reason) return WifiFailure::Association;
    return was_associated ? WifiFailure::NoIp : WifiFailure::Timeout;
}
void PublishFailure(WifiFailure failure, int reason = 0) {
    Publish([&](auto& v) {
        v.connecting = false; v.failure = failure; v.reason_code = reason;
        v.message = WifiFailureMessage(failure, reason);
    });
}
// STOP/down and event-loop fence delimit physical sessions; SDK events carry no
// request ID. Exact raw SSID/BSSID checks add peer ownership within that session.
bool CurrentPeer(const uint8_t* ssid, size_t size, const uint8_t* bssid) {
    if (!AcceptLinkEvents() || size > 32) return false;
    std::lock_guard<std::mutex> lock(link_mutex);
    return std::string(reinterpret_cast<const char*>(ssid), size) == active_ssid &&
        (!bssid_known || std::equal(active_bssid.begin(), active_bssid.end(), bssid));
}
void OnEvent(void*, esp_event_base_t base, int32_t event_id, void* data) {
    if (base == PLUS_WIFI_CONTROL) {
        if (data) barrier_done.store(*static_cast<const uint32_t*>(data));
        return;
    }
    if (base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_SCAN_DONE) scan_done.store(true);
        else if (event_id == WIFI_EVENT_STA_CONNECTED && data) {
            const auto* event = static_cast<const wifi_event_sta_connected_t*>(data);
            if (CurrentPeer(event->ssid, event->ssid_len, event->bssid)) {
                { std::lock_guard<std::mutex> lock(link_mutex);
                  std::copy_n(event->bssid, 6, active_bssid.begin()); bssid_known = true; }
                associated.store(true); fresh_ip.store(false);
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED && data) {
            const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
            if (CurrentPeer(event->ssid, event->ssid_len, event->bssid)) {
                int empty = 0;
                disconnect_reason.compare_exchange_strong(empty, event->reason);
                ClearLink();
                Publish([&](auto& v) {
                    if (!v.connecting) {
                        v.reason_code = disconnect_reason.load();
                        v.failure = ConnectionFailure(v.reason_code, false);
                        v.message = WifiFailureMessage(v.failure, v.reason_code);
                    }
                });
            }
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            stop_observed.store(true); radio_started.store(false); ClearLink();
        }
    } else if (base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP && data) {
            const auto* event = static_cast<ip_event_got_ip_t*>(data);
            if (AcceptLinkEvents() && event->esp_netif == netif && associated.load() && event->ip_info.ip.addr != 0) {
                if (!fresh_ip.exchange(true)) Publish([](auto& v) { ++v.connection_id; });
            }
        } else if (event_id == IP_EVENT_STA_LOST_IP && AcceptLinkEvents()) {
            fresh_ip.store(false);
            Publish([](auto& v) { v.connected = false; v.signal_bars = 0; v.ip.clear();
                if (!v.connecting) v.message = "Wi-Fi 地址已失效，请重新连接"; });
        }
    }
}

bool PrepareRadio() {
    // The standalone Plus weather service is the sole owner of the radio.
    // No legacy application, reconnect timer or board power policy is linked.
    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool initialized = esp_wifi_get_mode(&mode) == ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) netif = esp_netif_create_default_wifi_sta();
    if (!netif) return false;
    if (!initialized) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = false;
        if (esp_wifi_init(&config) != ESP_OK) return false;
    }
    if (!wifi_handler && esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            OnEvent, nullptr, &wifi_handler) != ESP_OK) return false;
    if (!ip_handler && esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
            OnEvent, nullptr, &ip_handler) != ESP_OK) return false;
    if (!barrier_handler && esp_event_handler_instance_register(PLUS_WIFI_CONTROL, 1,
            OnEvent, nullptr, &barrier_handler) != ESP_OK) return false;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
    stop_observed.store(false); radio_started.store(true);
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return true;
}

// Only the sole radio worker calls this. Stop the old STA control block and
// await the observed STOP/down state, then process an explicit event-loop fence.
// No fixed sleep is treated as proof that old connection events have drained.
bool StopRadioAndDrain(int64_t deadline, uint32_t request_generation, bool check_generation) {
    accept_link_events.store(false);
    if (radio_started.load()) {
        stop_observed.store(false);
        const auto stopped = esp_wifi_stop();
        if (stopped == ESP_ERR_WIFI_NOT_INIT || stopped == ESP_ERR_WIFI_NOT_STARTED)
            radio_started.store(false);
        else if (stopped != ESP_OK) return false;
    }
    auto expired = [&] {
        return esp_timer_get_time() >= deadline ||
               (check_generation && request_generation != generation.load());
    };
    auto actually_down = [&] {
        wifi_ap_record_t ap{};
        const auto state = esp_wifi_sta_get_ap_info(&ap);
        const bool station_down = state == ESP_ERR_WIFI_NOT_CONNECT ||
            state == ESP_ERR_WIFI_NOT_STARTED || state == ESP_ERR_WIFI_NOT_INIT ||
            (state == ESP_ERR_WIFI_CONN && stop_observed.load());
        return !radio_started.load() && station_down && (!netif || !esp_netif_is_netif_up(netif));
    };
    while (!actually_down()) {
        if (expired()) return false;
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    if (expired()) return false;
    if (!barrier_handler) {
        // Startup may have failed before a radio or barrier was created. That
        // failure must not trap manual editing/exit, but only exact NOT_INIT
        // plus actual down state proves there is no radio work to drain.
        wifi_mode_t mode = WIFI_MODE_NULL;
        wifi_ap_record_t ap{};
        if (!wifi_detail::UninitializedRadioIsStopped(
                esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT,
                esp_wifi_sta_get_ap_info(&ap) == ESP_ERR_WIFI_NOT_INIT,
                radio_started.load(), netif && esp_netif_is_netif_up(netif))) return false;
        associated.store(false); fresh_ip.store(false); disconnect_reason.store(0);
        ClearLink();
        return true;
    }
    const uint32_t ticket = barrier_next.fetch_add(1) + 1;
    if (esp_event_post(PLUS_WIFI_CONTROL, 1, &ticket, sizeof(ticket), 0) != ESP_OK) return false;
    while (barrier_done.load() != ticket) {
        if (expired()) return false;
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    if (expired() || !actually_down()) return false;
    associated.store(false); fresh_ip.store(false); disconnect_reason.store(0);
    ClearLink();
    return true;
}

bool RefreshLink(const std::string& expected) {
    wifi_ap_record_t ap{};
    esp_netif_ip_info_t ip{};
    const bool ap_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const std::string actual = ap_ok ? std::string(reinterpret_cast<const char*>(ap.ssid),
                                                 strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid))) : "";
    const bool up = netif && esp_netif_is_netif_up(netif);
    const bool ip_ok = netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
    const bool connected = wifi_detail::LinkReady(ap_ok && associated.load(), fresh_ip.load(),
                                                 up, ip_ok, expected, actual);
    char address[16]{};
    if (connected) esp_ip4addr_ntoa(&ip.ip, address, sizeof(address));
    Publish([&](auto& v) {
        // A disconnect/IP-loss callback may have run while the driver was
        // queried. Never overwrite its cleared state with this older sample.
        const bool current = connected && AcceptLinkEvents() && associated.load() && fresh_ip.load();
        const bool changed = v.connected != current || (current && v.ip != address);
        v.connected = current;
        v.signal_bars = current ? wifi_detail::SignalBars(ap.rssi) : 0;
        v.ip = current ? address : "";
        if (current) {
            v.ssid = actual;
            if (changed && !v.scanning) v.message = "Wi-Fi 已连接，已取得 IP 地址";
        } else if (changed && !v.scanning && !v.connecting) v.message = "Wi-Fi 已断开";
    });
    return connected && AcceptLinkEvents() && associated.load() && fresh_ip.load();
}

bool ConnectSaved(const std::string& ssid, uint32_t request_generation, int64_t deadline) {
    if (request_generation != generation.load()) return false;
    std::vector<WifiNetwork> networks;
    std::string error;
    if (!ReadApplicationWifi(&networks, &error)) {
        Publish([](auto& v) { v.connecting = false; v.failure = WifiFailure::Storage; v.reason_code = 0;
            v.message = "无法读取已保存的网络，请重新保存后重试"; });
        return false;
    }
    const auto found = std::find_if(networks.begin(), networks.end(), [&](const auto& n) { return n.ssid == ssid; });
    if (found == networks.end()) {
        Publish([](auto& v) { v.connecting = false; v.failure = WifiFailure::Storage; v.reason_code = 0;
            v.message = "网络尚未保存或已被忘记，请填写密码后连接"; });
        return false;
    }
    Publish([&](auto& v) {
        v.connecting = true; v.connected = false; v.signal_bars = 0; v.ip.clear();
        v.ssid = ssid; v.failure = WifiFailure::None; v.reason_code = 0;
        v.message = "配置已保存，正在连接 Wi-Fi";
    });
    if (!StopRadioAndDrain(std::min(deadline, esp_timer_get_time() + 3000000),
                           request_generation, true)) {
        if (request_generation == generation.load())
            PublishFailure(WifiFailure::Reset);
        return false;
    }
    if (request_generation != generation.load()) return false;
    const auto restarted = esp_wifi_start();
    if (restarted != ESP_OK) {
        PublishFailure(WifiFailure::Start, restarted);
        return false;
    }
    stop_observed.store(false); radio_started.store(true);
    if (request_generation != generation.load()) return false;
    wifi_config_t config{};
    memcpy(config.sta.ssid, found->ssid.data(), found->ssid.size());
    memcpy(config.sta.password, found->password.data(), found->password.size());
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    const auto configured = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    networks.clear();
    if (request_generation != generation.load()) return false;
    { std::lock_guard<std::mutex> lock(link_mutex);
      active_ssid = ssid; active_bssid = {}; bssid_known = false; }
    disconnect_reason.store(0);
    link_generation.store(request_generation);
    accept_link_events.store(true);
    const auto started = configured == ESP_OK ? esp_wifi_connect() : configured;
    if (started != ESP_OK) {
        accept_link_events.store(false);
        (void)esp_wifi_disconnect(); ClearLink();
        PublishFailure(WifiFailure::Start, started);
        return false;
    }
    while (esp_timer_get_time() < deadline) {
        if (request_generation != generation.load()) return false;
        if (RefreshLink(ssid)) {
            Publish([](auto& v) { v.connecting = false; v.failure = WifiFailure::None; v.reason_code = 0;
                v.message = "Wi-Fi 已连接，已取得 IP 地址"; });
            ESP_LOGI(kTag, "CONNECTED ip_valid=1");
            return true;
        }
        if (disconnect_reason.load() != 0) break;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (request_generation != generation.load()) return false;
    // Freeze failure before cleanup DISCONNECTED(8) can replace auth failure 202.
    const int reason = disconnect_reason.load();
    const auto failure = ConnectionFailure(reason, associated.load());
    accept_link_events.store(false);
    (void)esp_wifi_disconnect(); ClearLink();
    PublishFailure(failure, reason);
    ESP_LOGI(kTag, "CONNECT_FAILED reason=%d", reason);
    return false;
}

void Scan(uint32_t request_generation) {
    if (request_generation != generation.load()) { scan_pending.store(false); return; }
    Publish([](auto& v) { v.scanning = true; v.connecting = false; v.failure = WifiFailure::None; v.reason_code = 0;
        v.message = "正在搜索附近的 2.4 GHz Wi-Fi"; });
    scan_done.store(false);
    wifi_scan_config_t config{};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 80;
    config.scan_time.active.max = 160;
    bool ok = esp_wifi_scan_start(&config, false) == ESP_OK;
    const int64_t deadline = esp_timer_get_time() + kScanTimeoutUs;
    while (ok && !scan_done.load() && esp_timer_get_time() < deadline && request_generation == generation.load())
        vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && scan_done.load() && request_generation == generation.load();
    std::vector<WifiAccessPoint> points;
    if (ok) {
        // IDF returns scan records ordered by RSSI. Cap allocation; a user list
        // keeps at most 24 unique displayable SSIDs, including strongest BSSID.
        std::array<wifi_ap_record_t, 64> records{};
        static_assert(sizeof(records) <= 8192, "Wi-Fi records must leave at least half the worker stack free");
        uint16_t count = static_cast<uint16_t>(records.size());
        ok = esp_wifi_scan_get_ap_records(&count, records.data()) == ESP_OK;
        if (ok) for (uint16_t i = 0; i < count; ++i) {
            const auto& record = records[i];
            if (record.primary > 14) continue;
            points.push_back({std::string(reinterpret_cast<const char*>(record.ssid),
                                         strnlen(reinterpret_cast<const char*>(record.ssid), sizeof(record.ssid))),
                              record.rssi, record.authmode != WIFI_AUTH_OPEN});
        }
    } else (void)esp_wifi_scan_stop();
    // This also releases a completed scan's table on failure/cancellation.
    (void)esp_wifi_clear_ap_list();
    Publish([&](auto& v) {
        v.scanning = false;
        if (ok) {
            v.access_points = wifi_detail::VisibleAccessPoints(std::move(points));
            v.message = v.access_points.empty() ? "未发现可显示的网络，可重新搜索或手动输入名称" : "搜索完成，请选择网络后连接";
        } else if (request_generation == generation.load()) v.message = "搜索未完成，请稍后重试";
    });
    scan_pending.store(false);
}

void Worker(void*) {
    // Sole radio owner; this lightweight app has no legacy idle/audio policy.
    bool radio_ready = PrepareRadio();
    Publish([&](auto& v) { v.ready = radio_ready;
        v.message = radio_ready ? "Wi-Fi 已就绪" : "Wi-Fi 启动失败，请重新搜索后重试"; });
    std::string target;
    unsigned reconnect_attempts = 0;
    int64_t retry_at = 0;
    if (radio_ready) {
        std::vector<std::string> saved;
        {
            std::vector<WifiNetwork> entries; std::string error;
            if (ReadApplicationWifi(&entries, &error)) {
                for (auto i = entries.rbegin(); i != entries.rend(); ++i)
                    if (wifi_detail::ValidRawSsid(i->ssid)) saved.push_back(i->ssid);
            } else Publish([](auto& v) { v.message = "无法读取已保存网络，请检查 Wi-Fi 配置"; });
        }
        const auto boot_generation = generation.load();
        const int64_t total_deadline = esp_timer_get_time() + 30000000;
        for (const auto& ssid : saved) {
            // A user request submitted during radio initialization wins over
            // startup attempts, even if it arrived before this snapshot.
            if (boot_generation != generation.load() || uxQueueMessagesWaiting(queue.load()) != 0 ||
                esp_timer_get_time() >= total_deadline) break;
            if (ConnectSaved(ssid, boot_generation, std::min(total_deadline, esp_timer_get_time() + kConnectTimeoutUs))) {
                target = ssid; break;
            }
        }
        if (saved.empty()) Publish([](auto& v) { v.message = "尚无已保存网络，请搜索并连接"; });
    }
    Publish([](auto& v) { v.startup_complete = true; });
    Request request{};
    for (;;) {
        if (suspend_requested.load()) {
            if (!suspended.load()) {
                (void)esp_wifi_scan_stop();
                (void)esp_wifi_clear_ap_list();
                if (StopRadioAndDrain(esp_timer_get_time() + 3000000, generation.load(), false)) {
                    target.clear(); retry_at = 0; reconnect_attempts = 0;
                    radio_ready = false; scan_pending.store(false); ClearLink();
                    xQueueReset(queue.load());
                    Publish([](auto& v) { v.ready = false; v.scanning = false; v.connecting = false;
                        v.message = "Wi-Fi 已暂停"; });
                    suspended.store(true);
                }
            }
            vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(50)));
            continue;
        }
        if (suspended.exchange(false)) {
            radio_ready = PrepareRadio();
            Publish([&](auto& v) { v.ready = radio_ready;
                v.message = radio_ready ? "Wi-Fi 已恢复，请选择网络" : "Wi-Fi 恢复失败"; });
        }
        if (xQueueReceive(queue.load(), &request, pdMS_TO_TICKS(300)) == pdTRUE) {
            if (request.generation != generation.load()) {
                if (request.kind == Kind::Scan) scan_pending.store(false);
                continue;
            }
            if (request.kind == Kind::Disconnect) {
                target.clear(); retry_at = 0; reconnect_attempts = 0;
                (void)esp_wifi_disconnect(); ClearLink();
                Publish([](auto& v) { v.connecting = false; v.message = "Wi-Fi 已断开"; });
                continue;
            }
            if (!radio_ready || !radio_started.load()) {
                radio_ready = PrepareRadio();
                Publish([&](auto& v) { v.ready = radio_ready; });
            }
            if (!radio_ready) {
                scan_pending.store(false);
                Publish([](auto& v) { v.scanning = false; v.connecting = false;
                    v.message = "Wi-Fi 启动失败，请重启设备后重试"; });
                continue;
            }
            if (request.kind == Kind::Scan) Scan(request.generation);
            else {
                target.clear(); retry_at = 0; reconnect_attempts = 0;
                if (ConnectSaved(request.ssid, request.generation, esp_timer_get_time() + kConnectTimeoutUs)) target = request.ssid;
            }
        }
        if (radio_ready && !target.empty()) {
            if (RefreshLink(target)) { retry_at = 0; reconnect_attempts = 0; }
            else if (reconnect_attempts < 3) {
                if (!retry_at) retry_at = esp_timer_get_time() + 10000000;
                else if (esp_timer_get_time() >= retry_at && uxQueueMessagesWaiting(queue.load()) == 0) {
                    ++reconnect_attempts;
                    const auto current_generation = generation.load();
                    (void)ConnectSaved(target, current_generation, esp_timer_get_time() + kConnectTimeoutUs);
                    retry_at = esp_timer_get_time() + 30000000;
                }
            }
        }
    }
}

bool Submit(Kind kind, const std::string& ssid = {}) {
    std::lock_guard<std::mutex> lock(request_mutex);
    const auto q = queue.load();
    auto busy = [&] {
        Publish([&](auto& v) {
            // Rejected actions must not cancel the already running operation.
            v.failure = WifiFailure::QueueBusy; v.reason_code = 0;
            v.message = kind == Kind::Connect ? WifiFailureMessage(WifiFailure::QueueBusy) :
                "Wi-Fi 任务忙，请稍后重试";
        });
        return false;
    };
    if (!q || suspend_requested.load()) return busy();
    if (kind == Kind::Scan && scan_pending.load()) return true;
    if (uxQueueSpacesAvailable(q) == 0) return busy();
    Request request{};
    request.kind = kind;
    request.generation = kind == Kind::Scan ? generation.load() : generation.fetch_add(1) + 1;
    if (kind == Kind::Connect) memcpy(request.ssid, ssid.data(), ssid.size());
    if (kind == Kind::Scan) scan_pending.store(true);
    if (xQueueSend(q, &request, 0) == pdTRUE) {
        if (kind == Kind::Connect || kind == Kind::Disconnect) {
            // Hide the previous link immediately: the UI must not interpret
            // its cached IP as completion of this newly queued request.
            associated.store(false); fresh_ip.store(false);
            Publish([&](auto& v) {
                v.connected = false; v.signal_bars = 0; v.ip.clear();
                v.failure = WifiFailure::None; v.reason_code = 0;
                v.connecting = kind == Kind::Connect;
                if (kind == Kind::Connect) v.ssid = ssid;
                v.message = kind == Kind::Connect ? "配置已保存，正在连接 Wi-Fi" : "正在断开 Wi-Fi";
            });
        }
        return true;
    }
    if (kind == Kind::Scan) scan_pending.store(false);
    return busy();
}
}  // namespace

std::string WifiSsidLabel(const std::string& raw) {
    if (wifi_detail::DisplayableSsid(raw)) return raw;
    std::string display;
    for (unsigned char byte : raw) {
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') display += static_cast<char>(byte);
        else { char escaped[5]{}; std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte); display += escaped; }
    }
    return display;
}
std::string WifiFailureMessage(WifiFailure failure, int reason) {
    std::string message;
    switch (failure) {
    case WifiFailure::None: return {};
    case WifiFailure::Storage: message = "网络配置保存失败，请检查存储后重试"; break;
    case WifiFailure::QueueBusy: message = "配置已保存，但连接队列忙，请稍后重试"; break;
    case WifiFailure::Reset: message = "上次连接重置未完成，请稍后重试"; break;
    case WifiFailure::Authentication: message = "认证或握手失败，请检查密码后重试"; break;
    case WifiFailure::Association: message = "无法关联网络，请检查热点和信号"; break;
    case WifiFailure::NoIp: message = "已关联网络，但未取得 IP 地址，请检查路由器"; break;
    case WifiFailure::Start: message = "Wi-Fi 无法开始连接，请重试"; break;
    case WifiFailure::Timeout: message = "连接超时，请检查热点和信号后重试"; break;
    }
    if (reason) message += "（原因码 " + std::to_string(reason) + "）";
    return message;
}

void StartWifiConnection() {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (starting.exchange(true)) return;
    suspend_requested.store(false); suspended.store(false);
    const auto q = xQueueCreate(4, sizeof(Request));
    if (q) queue.store(q);
    if (!q || xTaskCreatePinnedToCoreWithCaps(Worker, "pocket_wifi", 16384, nullptr, 3,
            nullptr, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        queue.store(nullptr);
        if (q) vQueueDelete(q);
        Publish([](auto& v) { v.startup_complete = true; v.message = "内部内存不足，Wi-Fi 任务未启动"; });
        starting.store(false);
    }
}

WifiConnectionView WifiConnectionSnapshot() {
    std::lock_guard<std::mutex> lock(view_mutex);
    return view;
}
bool RequestWifiScan() { return Submit(Kind::Scan); }
bool RequestWifiConnect(const std::string& ssid) {
    return wifi_detail::ValidRawSsid(ssid) && Submit(Kind::Connect, ssid);
}
bool RequestWifiDisconnect() { return Submit(Kind::Disconnect); }
bool SuspendWifiConnection(uint32_t timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        if (!starting.load()) return true;
        if (!suspend_requested.exchange(true)) generation.fetch_add(1);
    }
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!suspended.load()) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    return true;
}
void ResumeWifiConnection() {
    std::lock_guard<std::mutex> lock(request_mutex);
    suspend_requested.store(false);
}
}  // namespace pocket
