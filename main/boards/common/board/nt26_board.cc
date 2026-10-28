#include "nt26_board.h"
#include "display.h"
#include "application.h"
#include "audio_codec.h"
#include "assets/lang_config.h"
#include "assets/lang_cellular.h"
#include "board.h"
#include "dual_network_board.h"

#include <esp_log.h>
#include <esp_network.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <chrono>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <font_awesome.h>
#include <cJSON.h>

#define TAG "Nt26Board"

namespace {
constexpr uint32_t kWaitNetworkConnected = (1 << 0);
constexpr uint32_t kWaitNetworkFailed = (1 << 1);
constexpr uint32_t kWaitNetworkAll =
    kWaitNetworkConnected | kWaitNetworkFailed;
constexpr uint32_t kNetworkWaitTimeoutMs = 60 * 1000;
/** P14 上升沿后模组冷启动 settle（再 AT） */
constexpr uint32_t kCellularBootSettleMs = 1000;
constexpr uint32_t kCellularRecoverStack = 6144;
constexpr int kRecoverWaitMs = 45000;

void StopCellularBeforeRestart() {
    auto* board = &Board::GetInstance();
    if (auto* dual = dynamic_cast<DualNetworkBoard*>(board)) board = &dual->GetCurrentBoard();
    if (auto* cellular = dynamic_cast<Nt26Board*>(board)) cellular->PreparePowerOff();
}
}  // namespace

Nt26Board::Nt26Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), ri_pin_(ri_pin) {
    ESP_ERROR_CHECK(esp_register_shutdown_handler(StopCellularBeforeRestart));
    // 勿用 ESP_INTR_FLAG_IRAM：OTA/NVS 写 Flash 时 Cache 关闭，若 GPIO ISR 仍跑
    // 会踩 Flash 映射区（Cache error）。无 IRAM 标志时驱动会在 Cache off 窗口屏蔽本 ISR。
    gpio_install_isr_service(0);
    esp_event_loop_create_default();
    esp_netif_init();
}

Nt26Board::~Nt26Board() {
    esp_unregister_shutdown_handler(StopCellularBeforeRestart);
    powered_.store(false);
    TearDownModem();
    while (signal_busy_.load()) vTaskDelay(pdMS_TO_TICKS(10));
}

std::string Nt26Board::GetBoardType() {
    return "nt26";
}

void Nt26Board::OnNetworkEvent(NetworkEvent event, const std::string& data, uint32_t expected_epoch) {
    // Only modem callbacks take this lock; synchronous startup calls already hold it.
    std::unique_lock<std::mutex> epoch_lock(lifecycle_mu_, std::defer_lock);
    if (expected_epoch != 0) {
        epoch_lock.lock();
        if (!powered_.load() || expected_epoch != lifecycle_epoch_.load()) return;
    }
    network_busy_.store(event == NetworkEvent::ModemDetecting || event == NetworkEvent::Connecting);
    if (event == NetworkEvent::Connected) {
        network_ready_.store(true, std::memory_order_release);
    } else if (event == NetworkEvent::Disconnected ||
               event == NetworkEvent::ModemErrorTimeout ||
               event == NetworkEvent::ModemErrorNoSim ||
               event == NetworkEvent::ModemErrorRegDenied ||
               event == NetworkEvent::ModemErrorInitFailed) {
        network_ready_.store(false, std::memory_order_release);
    }
    if (epoch_lock.owns_lock()) epoch_lock.unlock();
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void Nt26Board::SignalWaitBits(uint32_t bits) {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (network_wait_event_) xEventGroupSetBits(network_wait_event_, bits);
}

void Nt26Board::TearDownModem() {
    std::shared_ptr<UartEthModem> doomed;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        network_ready_.store(false, std::memory_order_release);
        network_busy_.store(false, std::memory_order_release);
        cached_csq_.store(99, std::memory_order_relaxed);
        lifecycle_epoch_.fetch_add(1, std::memory_order_acq_rel);
        // 唤醒 WaitBits；event group 仍由等待方释放，避免 UAF
        if (network_wait_event_) {
            xEventGroupSetBits(network_wait_event_, kWaitNetworkFailed);
        }
        doomed = std::move(modem_);
    }
    // Stop 在锁外：汇合 modem 任务时回调只 SignalWaitBits，不会与本锁死锁
    if (doomed) {
        ESP_LOGI(TAG, "TearDown modem (P14 / restart)");
        std::lock_guard<std::mutex> operation_lock(modem_operation_mu_);
        doomed->Stop();
    }
}

void Nt26Board::PreparePowerOff() {
    ESP_LOGI(TAG, "PreparePowerOff: stop cellular before P14 down");
    powered_.store(false, std::memory_order_release);
    TearDownModem();
    // 立即刷信号图标，避免状态栏仍显示旧 CSQ 直到下一秒 clock
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->UpdateStatusBar(true);
    }
}

bool Nt26Board::StartOrRestartModem(int timeout_ms) {
    // 整段单飞：EnsureNetworkReady 与 P14 上升沿恢复不得并行 TearDown 彼此
    const int budget_ms = timeout_ms > 0 ? timeout_ms : kNetworkWaitTimeoutMs;
    const int64_t deadline_us = esp_timer_get_time() + int64_t(budget_ms) * 1000;
    std::unique_lock<std::timed_mutex> start_lock(start_mu_, std::defer_lock);
    if (!start_lock.try_lock_for(std::chrono::milliseconds(budget_ms))) return IsNetworkReady();

    if (!powered_.load(std::memory_order_acquire)) return false;
    const int64_t settle_us = int64_t(kCellularBootSettleMs) * 1000 - (esp_timer_get_time() - power_restored_us_.load());
    if (settle_us > 0) {
        const int64_t available = deadline_us - esp_timer_get_time();
        if (available <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS((std::min(settle_us, available) + 999) / 1000));
        if (esp_timer_get_time() >= deadline_us) return false;
    }
    if (!powered_.load(std::memory_order_acquire)) return false;
    if (IsNetworkReady()) {
        return true;
    }

    bool existing = false;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        existing = modem_ != nullptr;
    }
    if (existing && (!retry_needed_.load() || esp_timer_get_time() - last_start_us_.load() < 5000000)) {
        while (powered_.load() && !IsNetworkReady() && !retry_needed_.load() && esp_timer_get_time() < deadline_us)
            vTaskDelay(pdMS_TO_TICKS(10));
        return IsNetworkReady();
    }
    TearDownModem();
    retry_needed_.store(false);
    last_start_us_.store(esp_timer_get_time());

    EventGroupHandle_t wait_eg = nullptr;
    uint32_t my_epoch = 0;
    std::shared_ptr<UartEthModem> started;

    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);

        if (network_ready_.load(std::memory_order_acquire) && modem_ &&
            modem_->IsInitialized()) {
            return true;
        }

        if (!powered_.load(std::memory_order_acquire)) return false;
        my_epoch = lifecycle_epoch_.load(std::memory_order_acquire);
        OnNetworkEvent(NetworkEvent::ModemDetecting);

        wait_eg = xEventGroupCreate();
        if (!wait_eg) {
            ESP_LOGE(TAG, "Failed to create network wait event group");
            OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
            return false;
        }
        network_wait_event_ = wait_eg;

        UartEthModem::Config config = {
            .uart_num = UART_NUM_2,
            .baud_rate = 2000000,
            .tx_pin = tx_pin_,
            .rx_pin = rx_pin_,
            .mrdy_pin = dtr_pin_,
            .srdy_pin = ri_pin_
        };

        modem_ = std::make_shared<UartEthModem>(config);
        modem_->SetNetworkEventCallback([this, my_epoch](UartEthModem::UartEthModemEvent event,
                                                const std::string& detail) {
            if (!powered_.load(std::memory_order_acquire) || my_epoch != lifecycle_epoch_.load(std::memory_order_acquire)) return;
            if (detail.empty()) {
                ESP_LOGI(TAG, "Modem event: %s", UartEthModem::GetNetworkEventName(event));
            } else {
                ESP_LOGI(TAG, "Modem event: %s (%s)",
                         UartEthModem::GetNetworkEventName(event), detail.c_str());
            }
            switch (event) {
                case UartEthModem::UartEthModemEvent::Connected:
                    OnNetworkEvent(NetworkEvent::Connected, "", my_epoch);
                    SignalWaitBits(kWaitNetworkConnected);
                    break;
                case UartEthModem::UartEthModemEvent::Disconnected:
                    OnNetworkEvent(NetworkEvent::Disconnected, "", my_epoch);
                    break;
                case UartEthModem::UartEthModemEvent::ErrorNoSim:
                    SignalWaitBits(kWaitNetworkFailed);
                    OnNetworkEvent(NetworkEvent::ModemErrorNoSim, "", my_epoch);
                    break;
                case UartEthModem::UartEthModemEvent::ErrorRegistrationDenied:
                    SignalWaitBits(kWaitNetworkFailed);
                    OnNetworkEvent(NetworkEvent::ModemErrorRegDenied, "", my_epoch);
                    break;
                case UartEthModem::UartEthModemEvent::Connecting:
                    OnNetworkEvent(NetworkEvent::Connecting, "", my_epoch);
                    break;
                case UartEthModem::UartEthModemEvent::ErrorInitFailed:
                    retry_needed_.store(true);
                    SignalWaitBits(kWaitNetworkFailed);
                    OnNetworkEvent(NetworkEvent::ModemErrorInitFailed, detail, my_epoch);
                    break;

            }
        });
        started = modem_;
    }

    // Serialize resource creation with power-off; Stop must not race Start.
    esp_err_t start_result;
    {
        std::lock_guard<std::mutex> operation_lock(modem_operation_mu_);
        start_result = powered_.load(std::memory_order_acquire) ? started->Start() : ESP_ERR_INVALID_STATE;
    }
    if (start_result != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mu_);
            if (network_wait_event_ == wait_eg) {
                network_wait_event_ = nullptr;
            }
            if (modem_ == started) {
                modem_.reset();
            }
        }
        vEventGroupDelete(wait_eg);
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return false;
    }

    ESP_LOGI(TAG, "Waiting for network ready (timeout=%d ms)...", timeout_ms);
    const int wait_ms = std::max(1, int((deadline_us - esp_timer_get_time()) / 1000));
    EventBits_t bits = xEventGroupWaitBits(wait_eg, kWaitNetworkAll, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(wait_ms));

    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        if (network_wait_event_ == wait_eg) {
            network_wait_event_ = nullptr;
        }
    }
    vEventGroupDelete(wait_eg);

    if (my_epoch != lifecycle_epoch_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "StartOrRestart aborted (P14 / newer lifecycle)");
        return false;
    }

    if (bits & kWaitNetworkConnected) {
        ESP_LOGI(TAG, "Network ready");
        return true;
    }

    ESP_LOGW(TAG, "Network wait failed or timed out (bits=0x%lx)", (unsigned long)bits);
    return false;
}

void Nt26Board::StartNetwork() {
    StartOrRestartModem(static_cast<int>(kNetworkWaitTimeoutMs));
}

bool Nt26Board::IsNetworkReady() {
    // 仅看 network_ready_：TearDown 先清标志再 reset modem_，避免无锁读 modem_ UAF
    return network_ready_.load(std::memory_order_acquire);
}

bool Nt26Board::EnsureNetworkReady(int timeout_ms) {
    if (IsNetworkReady()) return true;
    if (!powered_.load(std::memory_order_acquire)) return false;
    return StartOrRestartModem(timeout_ms);
}

void Nt26Board::CellularRecoverTask(void* arg) {
    auto* self = static_cast<Nt26Board*>(arg);

    if (!self->powered_.load(std::memory_order_acquire)) {
        ESP_LOGI(TAG, "P14 cellular recover skipped (rail off again)");
        self->recover_busy_.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    if (self->IsNetworkReady()) {
        ESP_LOGI(TAG, "P14 cellular recover skipped (already ready)");
        self->recover_busy_.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "P14 rising: re-init 4G modem");
    const bool ok = self->EnsureNetworkReady(kRecoverWaitMs);
    ESP_LOGI(TAG, "P14 cellular recover %s", ok ? "ok" : "failed");

    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->UpdateStatusBar(true);
    }

    self->recover_busy_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void Nt26Board::OnPowerRestored() {
    power_restored_us_.store(esp_timer_get_time());
    powered_.store(true, std::memory_order_release);
}

void Nt26Board::RecoverAfterPowerOnAsync() {
    if (!powered_.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!recover_busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "P14 cellular recover already pending");
        return;
    }
    if (IsNetworkReady()) {
        recover_busy_.store(false, std::memory_order_release);
        return;
    }
    if (xTaskCreate(&Nt26Board::CellularRecoverTask, "cell_recover",
                    kCellularRecoverStack, this, 5, nullptr) != pdPASS) {
        recover_busy_.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "P14 cellular recover task create failed");
    }
}

void Nt26Board::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

NetworkInterface* Nt26Board::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* Nt26Board::GetNetworkBusyStatusText() {
    return network_busy_.load() && !IsNetworkReady() ? Lang::Strings::REGISTERING_NETWORK : nullptr;
}

const char* Nt26Board::GetNetworkStateIcon() {
    if (!IsNetworkReady()) return FONT_AWESOME_SIGNAL_OFF;
    if (esp_timer_get_time() - signal_sample_us_.load() >= 5000000) {
        bool expected = false;
        if (signal_busy_.compare_exchange_strong(expected, true)) {
            if (xTaskCreate(SignalSampleTask, "cell_csq", 4096, this, 3, nullptr) != pdPASS)
                signal_busy_.store(false);
        }
    }
    const int csq = cached_csq_.load(std::memory_order_relaxed);
    if (csq == 99 || csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 9) {
        return FONT_AWESOME_SIGNAL_WEAK;
    } else if (csq >= 10 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_FAIR;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_GOOD;
    } else if (csq >= 20 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }
    return FONT_AWESOME_SIGNAL_OFF;
}

void Nt26Board::SignalSampleTask(void* arg) {
    auto* self = static_cast<Nt26Board*>(arg);
    if (self->powered_.load() && self->IsNetworkReady() &&
        Application::GetInstance().GetDeviceState() != kDeviceStateUpgrading) self->GetSignalStrength();
    self->signal_sample_us_.store(esp_timer_get_time());
    self->signal_busy_.store(false);
    vTaskDelete(nullptr);
}

void Nt26Board::SetPowerSaveMode(bool enabled) {
    (void)enabled;
}

std::string Nt26Board::GetBoardJson() {
    std::shared_ptr<UartEthModem> modem;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        modem = modem_;
    }
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", BOARD_TYPE);
    cJSON_AddStringToObject(root, "name", BOARD_NAME);
    if (modem) {
        cJSON_AddStringToObject(root, "revision", modem->GetModuleRevision().c_str());
        cJSON_AddStringToObject(root, "carrier", modem->GetCarrierName().c_str());
        cJSON_AddStringToObject(root, "csq", std::to_string(GetSignalStrength()).c_str());
        cJSON_AddStringToObject(root, "imei", modem->GetImei().c_str());
        cJSON_AddStringToObject(root, "iccid", modem->GetIccid().c_str());
        const auto cell = modem->GetCellInfo();
        auto* registration = cJSON_AddObjectToObject(root, "cereg");
        cJSON_AddNumberToObject(registration, "stat", cell.stat);
        if (!cell.tac.empty()) cJSON_AddStringToObject(registration, "tac", cell.tac.c_str());
        if (!cell.ci.empty()) cJSON_AddStringToObject(registration, "ci", cell.ci.c_str());
        if (cell.act >= 0) cJSON_AddNumberToObject(registration, "AcT", cell.act);
    } else {
        cJSON_AddStringToObject(root, "status", "offline");
    }
    char* encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded ? encoded : "{}";
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

int Nt26Board::GetSignalStrength() {
    std::shared_ptr<UartEthModem> modem;
    uint32_t epoch;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        modem = modem_;
        epoch = lifecycle_epoch_.load();
    }
    if (!modem) {
        return 99;
    }
    const int csq = modem->GetSignalStrength();
    if (epoch != lifecycle_epoch_.load() || !powered_.load()) return 99;
    cached_csq_.store(csq, std::memory_order_relaxed);
    signal_sample_us_.store(esp_timer_get_time());
    return csq;
}

esp_err_t Nt26Board::SendAtCommand(const std::string& cmd, std::string& response,
                                   uint32_t timeout_ms,
                                   bool bypass_init_check) {
    std::shared_ptr<UartEthModem> modem;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        modem = modem_;
        if (!modem) {
            ESP_LOGW(TAG, "SendAtCommand: modem 未实例化（WiFi 模式或 P14 已掉电）");
            return ESP_ERR_INVALID_STATE;
        }
        if (!bypass_init_check && !modem->IsInitialized()) {
            ESP_LOGW(TAG, "SendAtCommand: modem 尚未完成初始化");
            return ESP_ERR_INVALID_STATE;
        }
    }
    return modem->SendAt(cmd, response, timeout_ms);
}

esp_err_t Nt26Board::SendAtCommandCollectUntil(const std::string& cmd,
                                               std::string& response,
                                               uint32_t timeout_ms,
                                               const char* done_marker,
                                               bool bypass_init_check) {
    std::shared_ptr<UartEthModem> modem;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        modem = modem_;
        if (!modem) {
            ESP_LOGW(TAG, "SendAtCommandCollectUntil: modem 未实例化");
            return ESP_ERR_INVALID_STATE;
        }
        if (!bypass_init_check && !modem->IsInitialized()) {
            ESP_LOGW(TAG, "SendAtCommandCollectUntil: modem 尚未完成初始化");
            return ESP_ERR_INVALID_STATE;
        }
    }
    return modem->SendAtCollectUntil(cmd, response, timeout_ms, done_marker);
}

std::string Nt26Board::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) {
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    int battery_level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    std::shared_ptr<UartEthModem> modem;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        modem = modem_;
    }
    if (modem) {
        cJSON_AddStringToObject(network, "carrier", modem->GetCarrierName().c_str());
        int csq = modem->GetSignalStrength();
        if (csq == 99 || csq == -1) {
            cJSON_AddStringToObject(network, "signal", "unknown");
        } else if (csq >= 0 && csq <= 14) {
            cJSON_AddStringToObject(network, "signal", "weak");
        } else if (csq >= 15 && csq <= 24) {
            cJSON_AddStringToObject(network, "signal", "medium");
        } else if (csq >= 25 && csq <= 31) {
            cJSON_AddStringToObject(network, "signal", "strong");
        }
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
