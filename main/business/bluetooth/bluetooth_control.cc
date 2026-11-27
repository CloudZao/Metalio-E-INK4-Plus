#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "bluetooth_control.h"
#include "assets/lang_bluetooth.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/audio/bluetooth/simple_uart.hpp"
#include "boards/common/power/power_hw.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "BluetoothControl";
static std::atomic<BtMode> s_mode{BtMode::kMode1};
static std::atomic<ConnState> s_connection{ConnState::kIdle};
static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_view_active{false};
static std::atomic<int64_t> s_receive_until_us{0};
static std::mutex s_mutex;
static BluetoothControlSnapshot s_snapshot{1};
static std::string s_rx_buffer;
static bool s_rx_overflow_reported = false;
static std::string s_last_address;
static std::mutex audio_power_mutex;
static bool audio_power_held = false;
static bool mode3_audio = false;

static void SetStatusLocked(const char* text) {
    std::snprintf(s_snapshot.status, sizeof(s_snapshot.status), "%s", text ? text : "");
    ++s_snapshot.revision;
}

static void SetStatus(const char* text) {
    std::lock_guard<std::mutex> lock(s_mutex);
    SetStatusLocked(text);
}

static void SetAudioPowerLocked(bool enabled) {
    if (audio_power_held == enabled) {
        return;
    }
    if (enabled) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::BtAudio);
    } else {
        PowerPolicy::GetInstance().Release(PowerNeed::BtAudio);
    }
    audio_power_held = enabled;
}

static void SetModeAudioPower(BtMode mode) {
    std::lock_guard<std::mutex> lock(audio_power_mutex);
    mode3_audio = mode == BtMode::kMode3;
    SetAudioPowerLocked(mode3_audio);
}

void Bluetooth_release_page_audio_power() {
    s_view_active.store(false);
    std::lock_guard<std::mutex> lock(audio_power_mutex);
    if (!mode3_audio) {
        SetAudioPowerLocked(false);
    }
}

static bool Bluetooth_is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void Bluetooth_trim_line(std::string& line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    if (start > 0) {
        line = line.substr(start);
    }
}

static bool Bluetooth_parse_bt_device_line(const std::string& line, char* address, size_t addr_sz, char* name,
                                    size_t name_sz) {
    constexpr const char* kPrefix = "AT+BT:";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }
    const std::string payload = line.substr(strlen(kPrefix));
    if (payload.size() < static_cast<size_t>(kAddrHexLen)) {
        return false;
    }
    for (int i = 0; i < kAddrHexLen; ++i) {
        if (!Bluetooth_is_hex_char(payload[i])) {
            return false;
        }
    }
    snprintf(address, addr_sz, "%.*s", kAddrHexLen, payload.c_str());
    snprintf(name, name_sz, "%s", payload.c_str() + kAddrHexLen);
    return true;
}

static bool Send(const char* command, const char* stage) {
    if (!SimpleUart::getInstance().isInitialized()) {
        ESP_LOGW(TAG, "operation=command stage=%s result=uart_unavailable", stage);
        SetStatus(LangString_BT_UART_NOT_INIT);
        return false;
    }
    if (SimpleUart::getInstance().sendString(command)) {
        return true;
    }
    ESP_LOGW(TAG, "operation=command stage=%s result=send_failed", stage);
    SetStatus(LangString_BT_HID_SEND_FAILED);
    return false;
}

static bool ApplyMode(BtMode mode) {
    auto& uart = SimpleUart::getInstance();
    std::lock_guard<std::recursive_timed_mutex> transaction(uart.commandMutex());
    const char* before = nullptr;
    const char* command = nullptr;
    const char* status = nullptr;
    switch (mode) {
        case BtMode::kMode1:
            before = "AT+RX=2\r\n";
            command = "AT+MODE=1\r\n";
            status = LangString_BT_SWITCH_MODE1;
            break;
        case BtMode::kMode2:
            before = "AT+TX=1\r\n";
            command = "AT+MODE=2\r\n";
            status = LangString_BT_SWITCH_MODE2;
            break;
        case BtMode::kMode3:
            before = "AT+RX=1\r\n";
            command = "AT+MODE=3\r\n";
            status = LangString_BT_SWITCH_MODE3;
            break;
        case BtMode::kMode5:
            command = "AT+MODE=5\r\n";
            status = LangString_BT_SWITCH_MODE5;
            break;
        default:
            return false;
    }
    const auto previous = BluetoothCurrentMode();
    struct ModePowerGuard {
        BtMode previous;
        bool committed = false;
        ~ModePowerGuard() {
            if (!committed) {
                SetModeAudioPower(previous);
            }
        }
    } power_guard{previous};
    SetModeAudioPower(mode);
    SetStatus(status);
    if (before) {
        if (!Send(before, "prepare_mode")) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    if (!Send(command, "set_mode")) {
        return false;
    }
    power_guard.committed = true;
    return true;
}

static bool SubmitCommand(std::function<void()> command) try {
    if (s_busy.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (TaskExecutor::Schedule([command = std::move(command)] {
        struct BusyGuard {
            ~BusyGuard() {
                s_receive_until_us.store(esp_timer_get_time() + 3000000);
                s_busy.store(false, std::memory_order_release);
                std::lock_guard<std::mutex> lock(s_mutex);
                ++s_snapshot.revision;
            }
        } guard;
        PowerNeedHold hold(PowerNeed::StandbyInhibit);
        try {
            command();
        } catch (const std::bad_alloc&) {
            SetStatus(LangString_COMMON_MEMORY_RETRY);
            ESP_LOGE(TAG, "operation=command stage=execute result=no_memory");
        }
    }, TaskDomain::Network)) {
        return true;
    }
    s_busy.store(false, std::memory_order_release);
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    return false;
} catch (const std::bad_alloc&) {
    s_busy.store(false, std::memory_order_release);
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    ESP_LOGE(TAG, "operation=command stage=submit result=no_memory");
    return false;
}

bool Bluetooth_send_hid_shortcut(const char* shortcut) try {
    if (!shortcut || std::strlen(shortcut) > 32 || std::strpbrk(shortcut, "\r\n")) {
        return false;
    }
    const bool releasing = strcmp(shortcut, "ALT,UP") == 0 || strcmp(shortcut, "RELEASE") == 0;
    // 释放指令不得被模式切换或复位的忙标记拦截。
    if (!releasing) {
        if (s_busy.load(std::memory_order_acquire)) {
            return false;
        }
        if (s_mode != BtMode::kMode5) {
            SetStatus(LangString_BT_HID_NEED_MODE5);
            return false;
        }
    }
    auto& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        SetStatus(LangString_BT_UART_NOT_INIT);
        return false;
    }
    // 仅由固定按钮传入键名及按下、释放动作。
    const std::string command = std::string("AT+SHORTCUT=") + shortcut + "\r";
    SetStatus(LangString_BT_HID_WAITING);
    if (!uart.sendString(command)) {
        SetStatus(LangString_BT_HID_SEND_FAILED);
        ESP_LOGW(TAG, "HID shortcut TX failed");
        return false;
    }
    return true;
} catch (const std::bad_alloc&) {
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    ESP_LOGE(TAG, "operation=shortcut stage=allocate result=no_memory");
    return false;
}

bool Bluetooth_query_name() {
    auto& uart = SimpleUart::getInstance();
    return uart.isInitialized() && uart.sendString("AT+GLBN=1\r\n");
}

bool Bluetooth_is_valid_name(const char* name) {
    if (!name) {
        return false;
    }
    const size_t length = strlen(name);
    if (length == 0 || length > 63) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static void HandleLineLocked(const std::string& raw_line) {
    std::string line = raw_line;
    Bluetooth_trim_line(line);
    if (line.empty()) {
        return;
    }

    // 校准记录不进入页面日志或状态提示。
    if (line.rfind("AT+WDATA=", 0) == 0 || line.rfind("AT+RDATA=", 0) == 0 || line.rfind("AT+FLUSHDATA=", 0) == 0) {
        return;
    }

    if (line == "AT+SHORTCUT=OK") {
        SetStatusLocked(LangString_BT_HID_SUBMITTED);
        return;
    }
    if (line == "AT+SHORTCUT=ERR") {
        SetStatusLocked(LangString_BT_HID_INVALID);
        return;
    }
    if (line == "AT+SHORTCUT=DISCONNECTED") {
        SetStatusLocked(LangString_BT_HID_DISCONNECTED);
        return;
    }

    constexpr const char* kNamePrefix = "AT+LOCAL_NAME=";
    if (line.rfind(kNamePrefix, 0) == 0) {
        const std::string name = line.substr(strlen(kNamePrefix));
        // 改名响应可能回显整个设置命令；重启后的查询结果才是名称。
        if (!name.empty() && name.size() <= 63 && name.rfind("AT+SLBN=", 0) != 0 &&
            name.find_first_of("\r\n") == std::string::npos) {
            std::snprintf(s_snapshot.name, sizeof(s_snapshot.name), "%s", name.c_str());
            ++s_snapshot.revision;
        }
        return;
    }

    if (line.find("SET MODE 1") != std::string::npos) {
        s_mode = BtMode::kMode1;
        s_connection = ConnState::kIdle;
        SetStatusLocked(LangString_BT_MODE1_SET);
        return;
    }
    if (line.find("SET MODE 2") != std::string::npos) {
        s_mode = BtMode::kMode2;
        s_connection = ConnState::kIdle;
        SetStatusLocked(LangString_BT_MODE2_SET);
        return;
    }
    if (line.find("SET MODE 3") != std::string::npos) {
        s_mode = BtMode::kMode3;
        s_connection = ConnState::kIdle;
        SetStatusLocked(LangString_BT_MODE3_SET);
        return;
    }

    if (line.find("SET MODE 5") != std::string::npos) {
        s_mode = BtMode::kMode5;
        s_connection = ConnState::kIdle;
        SetStatusLocked(LangString_BT_MODE5_SET);
        return;
    }

    if (line.find("RECONNECT") != std::string::npos) {
        SetStatusLocked(line.c_str());
        return;
    }

    if (line.find("INQUIRING START") != std::string::npos) {
        s_connection = ConnState::kScanning;
        s_snapshot.devices.clear();
        ++s_snapshot.devices_revision;
        SetStatusLocked(LangString_BT_SCANNING);
        return;
    }

    char address[kAddrHexLen + 1];
    char name[64];
    if (Bluetooth_parse_bt_device_line(line, address, sizeof(address), name, sizeof(name))) {
        BtDevice dev{};
        snprintf(dev.address, sizeof(dev.address), "%s", address);
        snprintf(dev.name, sizeof(dev.name), "%s", name);
        const auto found = std::find_if(s_snapshot.devices.begin(), s_snapshot.devices.end(), [&](const auto& item) {
            return std::strcmp(item.address, dev.address) == 0;
        });
        if (found != s_snapshot.devices.end()) {
            *found = dev;
        } else if (s_snapshot.devices.size() < 64) {
            s_snapshot.devices.push_back(dev);
        }
        ++s_snapshot.devices_revision;
        char status[96];
        snprintf(status, sizeof(status), LangString_BT_FOUND_FMT, name[0] ? name : address);
        SetStatusLocked(status);
        return;
    }

    if (line.find("INQ COMPLETE") != std::string::npos) {
        s_connection = ConnState::kIdle;
        char status[64];
        snprintf(status, sizeof(status), LangString_BT_SCAN_DONE_FMT, static_cast<int>(s_snapshot.devices.size()));
        SetStatusLocked(status);
        return;
    }

    if (line.find("CONNECTING") != std::string::npos) {
        s_connection = ConnState::kConnecting;
        SetStatusLocked(LangString_BT_CONNECTING);
        return;
    }

    if (line.find("CONNECT SUCCESS") != std::string::npos) {
        s_connection = ConnState::kConnected;
        SetStatusLocked(LangString_BT_CONNECT_OK);
        return;
    }

    if (line.find("CONNECT TIMEOUT") != std::string::npos) {
        s_connection = ConnState::kIdle;
        SetStatusLocked(LangString_BT_CONNECT_TIMEOUT);
        return;
    }

    if (line.find("SETUP SCO") != std::string::npos) {
        SetStatusLocked(LangString_BT_CALL_MODE_SCO);
        return;
    }

    if (line.find("DISC SCO") != std::string::npos) {
        SetStatusLocked(LangString_BT_MUSIC_MODE_SCO);
        return;
    }

    SetStatusLocked(line.c_str());
}

static void OnUartData(const std::vector<uint8_t>& data) {
    if (data.empty() || (!s_view_active.load() && !s_busy.load() &&
                         esp_timer_get_time() >= s_receive_until_us.load())) {
        return;
    }
    const auto mode_before = BluetoothCurrentMode();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_rx_buffer.size() + data.size() > 2048) {
            s_rx_buffer.clear();
            if (!s_rx_overflow_reported) {
                s_rx_overflow_reported = true;
                ESP_LOGW(TAG, "operation=receive result=discard_overflow");
            }
            return;
        }
        s_rx_buffer.append(data.begin(), data.end());
        size_t consumed = 0;
        for (;;) {
            const auto end = s_rx_buffer.find('\n', consumed);
            if (end == std::string::npos) {
                break;
            }
            HandleLineLocked(s_rx_buffer.substr(consumed, end - consumed));
            consumed = end + 1;
        }
        s_rx_buffer.erase(0, consumed);
    }
    const auto mode = BluetoothCurrentMode();
    if (mode != mode_before) {
        SetModeAudioPower(mode);
    }
}

void BluetoothStartControl() {
    const bool active = s_view_active.exchange(true);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_rx_overflow_reported = false;
        if (!active && !s_busy.load()) {
            s_rx_buffer.clear();
        }
    }
    SimpleUart::getInstance().registerCallback(OnUartData);
}

bool BluetoothReadControl(BluetoothControlSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (snapshot.revision == s_snapshot.revision) {
        return false;
    }
    snapshot.devices = s_snapshot.devices;
    snapshot.devices_revision = s_snapshot.devices_revision;
    std::memcpy(snapshot.status, s_snapshot.status, sizeof(snapshot.status));
    std::memcpy(snapshot.name, s_snapshot.name, sizeof(snapshot.name));
    snapshot.revision = s_snapshot.revision;
    return true;
}

BtMode BluetoothCurrentMode() { return s_mode.load(std::memory_order_acquire); }
ConnState BluetoothConnectionState() { return s_connection.load(std::memory_order_acquire); }
bool BluetoothCommandsBusy() { return s_busy.load(std::memory_order_acquire); }

bool Bluetooth_send_mode_command(BtMode mode) {
    if (mode != BtMode::kMode1 && mode != BtMode::kMode2 && mode != BtMode::kMode3 && mode != BtMode::kMode5) {
        return false;
    }
    return SubmitCommand([mode] { ApplyMode(mode); });
}

bool Bluetooth_rename(const char* name) try {
    if (!Bluetooth_is_valid_name(name)) {
        SetStatus(LangString_BT_NAME_INVALID);
        return false;
    }
    if (!SimpleUart::getInstance().isInitialized()) {
        SetStatus(LangString_BT_UART_NOT_INIT);
        return false;
    }
    const auto mode = BluetoothCurrentMode();
    return SubmitCommand([name = std::string(name), mode] {
        auto& uart = SimpleUart::getInstance();
        std::lock_guard<std::recursive_timed_mutex> transaction(uart.commandMutex());
        if (!Send(("AT+SLBN=" + name + "\r\n").c_str(), "rename")) {
            SetStatus(LangString_BT_NAME_FAILED);
            return;
        }
        SetStatus(LangString_BT_NAME_RESTARTING);
        vTaskDelay(pdMS_TO_TICKS(700));
        const bool mode_ok = ApplyMode(mode);
        const bool name_ok = Bluetooth_query_name();
        if (!mode_ok || !name_ok) {
            SetStatus(LangString_BT_NAME_FAILED);
        }
    });
} catch (const std::bad_alloc&) {
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    return false;
}

bool BluetoothScanDevices() {
    if (BluetoothCurrentMode() != BtMode::kMode2) {
        SetStatus(LangString_BT_NEED_MODE2);
        return false;
    }
    return SubmitCommand([] {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_snapshot.devices.clear();
            ++s_snapshot.devices_revision;
        }
        if (Send("AT+INQUIRING\r\n", "scan")) {
            SetStatus(LangString_BT_SCAN_START);
        }
    });
}

bool BluetoothConnectDevice(const char* address) try {
    if (!address || std::strlen(address) != kAddrHexLen ||
        !std::all_of(address, address + kAddrHexLen, Bluetooth_is_hex_char)) {
        return false;
    }
    return SubmitCommand([address = std::string(address)] {
        if (Send(("AT+CONNECT=" + address + "\r\n").c_str(), "connect")) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_last_address = address;
            }
            s_connection.store(ConnState::kConnecting);
            SetStatus(LangString_BT_CONNECTING);
        }
    });
} catch (const std::bad_alloc&) {
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    return false;
}

bool BluetoothSetCallMode(bool call) {
    if (BluetoothConnectionState() != ConnState::kConnected) {
        SetStatus(LangString_BT_NEED_CONNECT);
        return false;
    }
    return SubmitCommand([call] {
        {
            std::lock_guard<std::mutex> lock(audio_power_mutex);
            SetAudioPowerLocked(mode3_audio || (call && s_view_active.load()));
        }
        auto& uart = SimpleUart::getInstance();
        std::lock_guard<std::recursive_timed_mutex> transaction(uart.commandMutex());
        SetStatus(call ? LangString_BT_SWITCH_CALL : LangString_BT_SWITCH_MUSIC);
        if (Send(call ? "AT+PP=1\r\n" : "AT+BTSCO=0\r\n", "audio_prepare")) {
            vTaskDelay(pdMS_TO_TICKS(200));
            Send(call ? "AT+BTSCO=1\r\n" : "AT+PP=1\r\n", "audio_mode");
        }
    });
}

bool BluetoothResetModule(std::function<bool()> ready) try {
    return SubmitCommand([ready = std::move(ready)] {
        PowerNeedHold reset(PowerNeed::PeripheralReset);
        if (!ready || !ready()) {
            SetStatus(LangString_BT_PWR_RESET_FAILED);
            return;
        }
        const auto mode = BluetoothCurrentMode() == BtMode::kNone ? BtMode::kMode1 : BluetoothCurrentMode();
        std::string address;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            address = s_last_address;
        }
        s_mode.store(BtMode::kNone);
        s_connection.store(ConnState::kIdle);
        SetModeAudioPower(BtMode::kNone);
        const auto error = power_hw_bt_power_reset();
        if (error != ESP_OK || !SimpleUart::getInstance().isInitialized()) {
            ESP_LOGE(TAG, "operation=reset result=failed error=%d", int(error));
            SetStatus(LangString_BT_PWR_RESET_FAILED);
            return;
        }
        SetStatus(LangString_BT_PWR_RESET_RECONNECT);
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!ApplyMode(mode)) {
            SetStatus(LangString_BT_PWR_RESET_FAILED);
            return;
        }
        s_mode.store(mode);
        if (mode == BtMode::kMode2 && !address.empty()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (Send(("AT+CONNECT=" + address + "\r\n").c_str(), "reconnect")) {
                s_connection.store(ConnState::kConnecting);
            }
        }
        SetStatus(mode == BtMode::kMode2 && !address.empty() ? LangString_BT_PWR_RESET_RECONNECT : LangString_BT_PWR_RESET_OK);
    });
} catch (const std::bad_alloc&) {
    SetStatus(LangString_COMMON_MEMORY_RETRY);
    ESP_LOGE(TAG, "operation=reset stage=submit result=no_memory");
    return false;
}
