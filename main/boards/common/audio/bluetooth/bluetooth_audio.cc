#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "bluetooth_audio.h"
#include "infra/tasks/task_create.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "boards/common/power/power_hw.h"
#include "config.h"
#include "simple_uart.hpp"
#include "sysmgmt/power/policy/power_policy.h"

static constexpr const char* TAG = "BluetoothAudio";

static BluetoothAudioConfig s_config{};

static constexpr int kRetries = 3;                    // 首次失败后的最大重试次数
static constexpr int kAckTimeoutMs = 1000;            // 模式成功应答等待上限
static constexpr int64_t kBootSettleUs = 700000;      // 重新上电后的稳定时间
static std::atomic<bool> s_powered{true};             // 蓝牙芯片供电状态
static std::atomic<unsigned> s_power_epoch{0};        // 当前电源周期
static std::atomic<unsigned> s_ready_epoch{~0U};      // 已确认模式成功的电源周期
static std::atomic<unsigned> s_completed_epoch{~0U};  // 本轮重试已结束的电源周期
static std::atomic<int64_t> s_power_restored_us{0};   // 最近一次恢复供电时间
static std::atomic<bool> s_requested{false};          // 初始化任务已排队或正在执行
static std::atomic<TaskHandle_t> s_worker{nullptr};   // 模式初始化后台任务
void (*s_boot_reset_done)() = nullptr;                // 启动复位结束后通知板级调度
static bool s_codec_started = false;                  // 初始输出音量已设置
static std::mutex s_reply_mutex;                      // 保护跨任务应答状态
static char s_reply_line[128]{};                      // 当前串口应答行
static size_t s_reply_length = 0;                     // 当前应答行长度
static bool s_reply_overflow = false;                 // 当前应答行超长
static bool s_applying = false;                       // 正在执行模式配置
static bool s_expect_mode = false;                    // 已发送模式命令并等待确认
static bool s_mode_ack = false;                       // 已收到模式1成功应答
static bool s_command_error = false;                  // 本次配置收到失败应答
static char s_bt_version[64] = {};                    // 最近收到的有效版本，由应答互斥锁保护

static void OnUartData(const std::vector<uint8_t>& data) {
    if (s_config.on_uart_data) {
        s_config.on_uart_data(data);
    }
    std::lock_guard<std::mutex> lock(s_reply_mutex);
    for (uint8_t c : data) {
        if (c == '\r' || c == '\n') {
            s_reply_line[s_reply_length] = 0;
            if (!s_reply_overflow) {
                const char* reply = s_reply_line;
                while (*reply == ' ') {
                    ++reply;
                }
                size_t reply_length = strlen(reply);
                while (reply_length > 0 && reply[reply_length - 1] == ' ') {
                    --reply_length;
                }
                constexpr const char* version_prefix = "AT+GVER=";
                const size_t prefix_length = strlen(version_prefix);
                if (reply_length > prefix_length && strncmp(reply, version_prefix, prefix_length) == 0) {
                    const char* version = reply + prefix_length;
                    const size_t version_length = reply_length - prefix_length;
                    if (version_length > 1 && version_length < sizeof(s_bt_version) && version[0] == 'V' &&
                        strspn(version, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_+") ==
                            version_length) {
                        memcpy(s_bt_version, version, version_length);
                        s_bt_version[version_length] = '\0';
                    }
                }
            }
            if (!s_reply_overflow && s_applying) {
                const char* mode = strstr(s_reply_line, "SET MODE 1");
                if (s_expect_mode && mode && (mode[10] == 0 || mode[10] == ' ')) {
                    s_mode_ack = true;
                }
                if (strstr(s_reply_line, "ERROR") || strstr(s_reply_line, "FAIL")) {
                    s_command_error = true;
                }
            }
            s_reply_length = 0;
            s_reply_overflow = false;
        } else if (c >= 32 && c < 127) {
            if (s_reply_length + 1 < sizeof(s_reply_line)) {
                s_reply_line[s_reply_length++] = char(c);
            } else {
                s_reply_overflow = true;
            }
        }
    }
}

static bool PowerUnchanged(unsigned epoch) {
    return s_powered.load() && s_power_epoch.load() == epoch;
}

static bool ApplyMode(unsigned epoch) {
    auto& uart = SimpleUart::getInstance();
    std::lock_guard<std::recursive_timed_mutex> transaction(uart.commandMutex());

    struct ReplyScope {
        ~ReplyScope() {
            std::lock_guard<std::mutex> lock(s_reply_mutex);
            s_applying = false;
            s_expect_mode = false;
        }
    } scope;

    {
        std::lock_guard<std::mutex> lock(s_reply_mutex);
        s_expect_mode = false;
        s_mode_ack = false;
        s_command_error = false;
        s_applying = true;
        s_reply_length = 0;
        s_reply_overflow = false;
    }
    if (!PowerUnchanged(epoch) || !uart.sendString("AT+RX=2\r\n")) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(700));
    if (!PowerUnchanged(epoch)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_reply_mutex);
        if (s_command_error) {
            return false;
        }
        s_reply_length = 0;
        s_reply_overflow = false;
        s_expect_mode = true;
    }
    if (!uart.sendString("AT+MODE=1\r\n")) {
        return false;
    }
    const int64_t deadline = esp_timer_get_time() + kAckTimeoutMs * 1000;
    while (PowerUnchanged(epoch) && esp_timer_get_time() < deadline) {
        {
            std::lock_guard<std::mutex> lock(s_reply_mutex);
            if (s_command_error) {
                return false;
            }
            if (s_mode_ack) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void NotifyBootResetDone() {
    const auto callback = s_boot_reset_done;
    s_boot_reset_done = nullptr;
    if (callback) {
        callback();
    }
}

static void ModeWorker(void*) {
    bool boot_reset = s_config.reset_on_boot;
    if (!boot_reset) {
        NotifyBootResetDone();
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        PowerNeedHold power(PowerNeed::PeripheralReset);
        unsigned epoch = s_power_epoch.load();
        bool success = false;
        for (int attempt = 0; attempt <= kRetries; ++attempt) {
            const char* stage = "reset";
            bool reset_ok = true;
            if (boot_reset) {
                reset_ok = power_hw_bt_power_reset() == ESP_OK;
                epoch = s_power_epoch.load();
                if (reset_ok) {
                    boot_reset = false;
                    NotifyBootResetDone();
                }
            }
            if (reset_ok) {
                if (!PowerUnchanged(epoch)) {
                    break;
                }
                const int64_t settle = kBootSettleUs - (esp_timer_get_time() - s_power_restored_us.load());
                if (settle > 0) {
                    vTaskDelay(pdMS_TO_TICKS((settle + 999) / 1000));
                }
                if (ApplyMode(epoch)) {
                    success = true;
                    break;
                }
                stage = "mode_ack";
            }
            ESP_LOGW(TAG, "Audio initialization failed stage=%s attempt=%d/%d", stage, attempt + 1, kRetries + 1);
            if (attempt == kRetries || (reset_ok && !PowerUnchanged(epoch))) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (s_power_epoch.load() == epoch) {
            s_ready_epoch.store(success ? epoch : ~0U);
            // 同一电源周期失败后不因轮询重新启动一轮重试。
            s_completed_epoch.store(epoch);
        }
        NotifyBootResetDone();
        s_requested.store(false);
        if (!success) {
            ESP_LOGE(TAG, "Audio mode unavailable after initialization");
        }
    }
}

AudioCodec* BluetoothAudio_GetCodec() {
    return Board::GetInstance().GetAudioCodec();
}

void BluetoothAudio_Init(const BluetoothAudioConfig& config, void (*on_boot_reset_done)()) {
    s_config = config;
    s_boot_reset_done = on_boot_reset_done;
    BluetoothAudio_SetPa(false);
    auto& uart = SimpleUart::getInstance();
    if (!uart.begin(BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, 115200, config.uart_port)) {
        s_completed_epoch.store(s_power_epoch.load());
        ESP_LOGE(TAG, "BT UART initialization failed");
        NotifyBootResetDone();
        return;
    }
    uart.registerObserver(OnUartData);
    TaskHandle_t worker = nullptr;
    if (FirmwareCreateTask(ModeWorker, "bt_mode_init", 4096, nullptr, 5, &worker, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_completed_epoch.store(s_power_epoch.load());
        ESP_LOGE(TAG, "Audio mode worker creation failed");
        NotifyBootResetDone();
        return;
    }
    s_worker.store(worker);
    BluetoothAudio_RequestModeInit();
}

void BluetoothAudio_RequestModeInit() {
    const auto worker = s_worker.load();
    if (!worker || !s_powered.load() || s_completed_epoch.load() == s_power_epoch.load()) {
        return;
    }
    bool expected = false;
    if (s_requested.compare_exchange_strong(expected, true)) {
        xTaskNotifyGive(worker);
    }
}

void BluetoothAudio_OnPowerOff() {
    s_powered.store(false);
    s_power_epoch.fetch_add(1);
    s_ready_epoch.store(~0U);
    s_completed_epoch.store(~0U);
}

void BluetoothAudio_OnPowerRestored() {
    s_power_restored_us.store(esp_timer_get_time());
    s_powered.store(true);
}

bool BluetoothAudio_WaitModeReady(int timeout_ms, std::function<bool()> still_needed) {
    if (still_needed && !still_needed()) {
        return false;
    }
    BluetoothAudio_RequestModeInit();
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    while (!BluetoothAudio_ModeReady()) {
        if (still_needed && !still_needed()) {
            return false;
        }
        if (!s_worker.load() || s_completed_epoch.load() == s_power_epoch.load() || esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        BluetoothAudio_RequestModeInit();
    }
    return !still_needed || still_needed();
}

bool BluetoothAudio_EnsureStarted() {
    AudioCodec* codec = BluetoothAudio_GetCodec();
    if (codec == nullptr) {
        return false;
    }
    if (!BluetoothAudio_ModeReady()) {
        BluetoothAudio_RequestModeInit();
        return false;
    }
    if (!s_codec_started) {
        codec->SetOutputVolume(80);
        s_codec_started = true;
    }
    codec->Start();
    return codec->HardwareReady();
}

bool BluetoothAudio_ModeReady() {
    return s_powered.load() && s_ready_epoch.load() == s_power_epoch.load();
}

BluetoothAudioState BluetoothAudio_GetState() {
    if (!s_powered.load()) {
        return BluetoothAudioState::NotReady;
    }
    const unsigned epoch = s_power_epoch.load();
    if (s_ready_epoch.load() == epoch) {
        return BluetoothAudioState::Ready;
    }
    if (s_completed_epoch.load() == epoch) {
        return BluetoothAudioState::Failed;
    }
    return s_requested.load() ? BluetoothAudioState::Initializing : BluetoothAudioState::NotReady;
}

esp_err_t BluetoothAudio_QueryVersion() {
    auto& uart = SimpleUart::getInstance();
    std::unique_lock<std::recursive_timed_mutex> transaction(uart.commandMutex(), std::try_to_lock);
    if (!transaction.owns_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_powered.load() || !uart.isInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        std::lock_guard<std::mutex> lock(s_reply_mutex);
        s_bt_version[0] = '\0';
    }
    if (!uart.sendString("AT+GVER=1\r\n")) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool BluetoothAudio_GetVersion(char* version, size_t capacity) {
    if (version == nullptr || capacity == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_reply_mutex);
    const size_t length = strlen(s_bt_version);
    if (length == 0 || length >= capacity) {
        version[0] = '\0';
        return false;
    }
    memcpy(version, s_bt_version, length + 1);
    return true;
}

esp_err_t BluetoothAudio_SetPa(bool on) {
    return s_config.set_pa ? s_config.set_pa(on) : ESP_ERR_INVALID_STATE;
}

int BluetoothAudio_InputRate() {
    AudioCodec* codec = BluetoothAudio_GetCodec();
    return codec != nullptr ? codec->input_sample_rate() : AUDIO_INPUT_SAMPLE_RATE;
}

int BluetoothAudio_ReadMic(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0 || !BluetoothAudio_EnsureStarted()) {
        return -1;
    }
    AudioCodec* codec = BluetoothAudio_GetCodec();
    std::vector<int16_t> buf(static_cast<size_t>(samples));
    if (!codec->InputData(buf)) {
        return 0;
    }
    int got = static_cast<int>(buf.size());
    std::memcpy(dest, buf.data(), static_cast<size_t>(got) * sizeof(int16_t));
    return got;
}

int BluetoothAudio_WriteSpk(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0 || !BluetoothAudio_EnsureStarted()) {
        return -1;
    }
    BluetoothAudio_SetPa(true);
    AudioCodec* codec = BluetoothAudio_GetCodec();
    std::vector<int16_t> buf(data, data + samples);
    codec->OutputData(buf);
    return samples;
}
