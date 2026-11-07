#include "bluetooth_uart.h"

#include "bluetooth_screen_priv.h"
#include "bluetooth_screen_ui.h"
#include "assets/lang_config.h"
#include "assets/lang_bluetooth.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "settings_common.h"
#include "screen_common.h"
#include "screen_text.h"
#include "simple_uart.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
std::mutex audio_power_mutex;
bool audio_power_held = false;
bool mode3_audio = false;

void SetAudioPowerLocked(bool enabled) {
    if (audio_power_held == enabled) return;
    if (enabled) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::BtAudio);
    } else {
        PowerPolicy::GetInstance().Release(PowerNeed::BtAudio);
    }
    audio_power_held = enabled;
}
}  // namespace

void Bluetooth_set_mode_audio_power(BtMode mode) {
    std::lock_guard<std::mutex> lock(audio_power_mutex);
    mode3_audio = mode == BtMode::kMode3;
    SetAudioPowerLocked(mode3_audio);
}

void Bluetooth_release_page_audio_power() {
    std::lock_guard<std::mutex> lock(audio_power_mutex);
    if (!mode3_audio) SetAudioPowerLocked(false);
}

bool Bluetooth_is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void Bluetooth_trim_line(std::string& line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
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

bool Bluetooth_parse_bt_device_line(const std::string& line, char* address, size_t addr_sz,
                                 char* name, size_t name_sz) {
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

bool Bluetooth_query_version() {
    auto& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "version query: UART not initialized");
        Bluetooth_post_status(Lang::Strings::BT_UART_NOT_INIT);
        return false;
    }
    if (!uart.sendString("AT+GVER=1\r\n")) {
        ESP_LOGW(TAG, "TX failed: AT+GVER=1");
        return false;
    }
    ESP_LOGI(TAG, "TX: AT+GVER=1");
    return true;
}

bool Bluetooth_send_hid_shortcut(const char* shortcut) {
    const bool releasing = strcmp(shortcut, "ALT,UP") == 0 || strcmp(shortcut, "RELEASE") == 0;
    // 释放指令不得被模式切换或复位的忙标记拦截。
    if (!releasing) {
        if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return false;
        if (Bluetooth_active_mode != BtMode::kMode5) {
            Bluetooth_post_status(Lang::Strings::BT_HID_NEED_MODE5);
            return false;
        }
    }
    auto& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        Bluetooth_post_status(Lang::Strings::BT_UART_NOT_INIT);
        return false;
    }
    // 仅由固定按钮传入键名及按下、释放动作。
    const std::string command = std::string("AT+SHORTCUT=") + shortcut + "\r";
    Bluetooth_post_status(Lang::Strings::BT_HID_WAITING);
    if (!uart.sendString(command)) {
        Bluetooth_post_status(Lang::Strings::BT_HID_SEND_FAILED);
        ESP_LOGW(TAG, "HID shortcut TX failed");
        return false;
    }
    return true;
}

bool Bluetooth_query_name() {
    auto& uart = SimpleUart::getInstance();
    return uart.isInitialized() && uart.sendString("AT+GLBN=1\r\n");
}

namespace {
struct RenameArgs {
    char name[64]; // 待设置名称，包含结束符。
    BtMode mode; // 模块重启后恢复的模式。
};

void RenameTask(void* param) {
    auto* args = static_cast<RenameArgs*>(param);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    auto& uart = SimpleUart::getInstance();
    if (uart.sendString(std::string("AT+SLBN=") + args->name + "\r\n")) {
        Bluetooth_post_status(Lang::Strings::BT_NAME_RESTARTING);
        vTaskDelay(pdMS_TO_TICKS(700));
        Bluetooth_apply_mode_commands(args->mode);
        if (!Bluetooth_query_name()) Bluetooth_post_status(Lang::Strings::BT_NAME_FAILED);
    } else {
        Bluetooth_post_status(Lang::Strings::BT_NAME_FAILED);
    }
    PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
    delete args;
    Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}
} // namespace

bool Bluetooth_is_valid_name(const char* name) {
    const size_t length = strlen(name);
    if (length == 0 || length > 63) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool Bluetooth_rename(const char* name) {
    if (!Bluetooth_is_valid_name(name)) {
        Bluetooth_post_status(Lang::Strings::BT_NAME_INVALID);
        return false;
    }
    if (!SimpleUart::getInstance().isInitialized()) {
        Bluetooth_post_status(Lang::Strings::BT_UART_NOT_INIT);
        return false;
    }
    bool expected = false;
    if (!Bluetooth_mode_cmd_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    auto* args = new (std::nothrow) RenameArgs{};
    if (args != nullptr) {
        snprintf(args->name, sizeof(args->name), "%s", name);
        args->mode = Bluetooth_active_mode.load();
    }
    if (args == nullptr || xTaskCreate(RenameTask, "bt_rename", 4096, args, 5, nullptr) != pdPASS) {
        delete args;
        Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
        Bluetooth_post_status(Lang::Strings::BT_NAME_FAILED);
        return false;
    }
    return true;
}

void Bluetooth_handle_response_line(const std::string& raw_line) {
    std::string line = raw_line;
    Bluetooth_trim_line(line);
    if (line.empty()) {
        return;
    }

    ESP_LOGI(TAG, "RX: %s", line.c_str());

    if (line == "AT+SHORTCUT=OK") {
        Bluetooth_post_status(Lang::Strings::BT_HID_SUBMITTED);
        return;
    }
    if (line == "AT+SHORTCUT=ERR") {
        Bluetooth_post_status(Lang::Strings::BT_HID_INVALID);
        return;
    }
    if (line == "AT+SHORTCUT=DISCONNECTED") {
        Bluetooth_post_status(Lang::Strings::BT_HID_DISCONNECTED);
        return;
    }

    constexpr const char* kNamePrefix = "AT+LOCAL_NAME=";
    if (line.rfind(kNamePrefix, 0) == 0) {
        const std::string name = line.substr(strlen(kNamePrefix));
        // 改名响应可能回显整个设置命令；重启后的查询结果才是名称。
        if (!name.empty() && name.size() <= 63 && name.rfind("AT+SLBN=", 0) != 0 &&
            name.find_first_of("\r\n") == std::string::npos) {
            Bluetooth_post_name(name.c_str());
        }
        return;
    }

    constexpr const char* kVersionPrefix = "AT+GVER=";
    if (line.rfind(kVersionPrefix, 0) == 0) {
        std::string version = line.substr(strlen(kVersionPrefix));
        Bluetooth_trim_line(version);
        if (version.size() > 1 && version.size() < 64 && version[0] == 'V' &&
            version.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_+") ==
                std::string::npos) {
            Bluetooth_post_version(version.c_str());
        }
        return;
    }

    if (line.find("SET MODE 1") != std::string::npos) {
        Bluetooth_active_mode = BtMode::kMode1;
        Bluetooth_conn_state = ConnState::kIdle;
        Bluetooth_post_status(Lang::Strings::BT_MODE1_SET);
        ScreenLvAsync(Bluetooth_async_on_mode1_set);
        return;
    }
    if (line.find("SET MODE 2") != std::string::npos) {
        Bluetooth_active_mode = BtMode::kMode2;
        Bluetooth_conn_state = ConnState::kIdle;
        Bluetooth_post_status(Lang::Strings::BT_MODE2_SET);
        ScreenLvAsync(Bluetooth_async_on_mode2_set);
        return;
    }
    if (line.find("SET MODE 3") != std::string::npos) {
        Bluetooth_active_mode = BtMode::kMode3;
        Bluetooth_conn_state = ConnState::kIdle;
        Bluetooth_post_status(Lang::Strings::BT_MODE3_SET);
        ScreenLvAsync(Bluetooth_async_on_mode3_set);
        return;
    }

    if (line.find("SET MODE 5") != std::string::npos) {
        Bluetooth_active_mode = BtMode::kMode5;
        Bluetooth_conn_state = ConnState::kIdle;
        Bluetooth_post_status(Lang::Strings::BT_MODE5_SET);
        ScreenLvAsync([](void*) { Bluetooth_restore_mode_ui(); });
        return;
    }

    if (line.find("RECONNECT") != std::string::npos) {
        Bluetooth_post_status(line.c_str());
        return;
    }

    if (line.find("INQUIRING START") != std::string::npos) {
        Bluetooth_conn_state = ConnState::kScanning;
        Bluetooth_devices.clear();
        Bluetooth_post_clear_list();
        Bluetooth_post_status(Lang::Strings::BT_SCANNING);
        return;
    }

    char address[kAddrHexLen + 1];
    char name[64];
    if (Bluetooth_parse_bt_device_line(line, address, sizeof(address), name, sizeof(name))) {
        BtDevice dev{};
        snprintf(dev.address, sizeof(dev.address), "%s", address);
        snprintf(dev.name, sizeof(dev.name), "%s", name);
        Bluetooth_devices.push_back(dev);
        Bluetooth_add_device_to_list(address, name);
        char status[96];
        snprintf(status, sizeof(status), Lang::Strings::BT_FOUND_FMT, name[0] ? name : address);
        Bluetooth_post_status(status);
        return;
    }

    if (line.find("INQ COMPLETE") != std::string::npos) {
        Bluetooth_conn_state = ConnState::kIdle;
        char status[64];
        snprintf(status, sizeof(status), Lang::Strings::BT_SCAN_DONE_FMT,
                 static_cast<int>(Bluetooth_devices.size()));
        Bluetooth_post_status(status);
        return;
    }

    if (line.find("CONNECTING") != std::string::npos) {
        Bluetooth_conn_state = ConnState::kConnecting;
        Bluetooth_post_status(Lang::Strings::BT_CONNECTING);
        return;
    }

    if (line.find("CONNECT SUCCESS") != std::string::npos) {
        Bluetooth_conn_state = ConnState::kConnected;
        Bluetooth_post_status(Lang::Strings::BT_CONNECT_OK);
        return;
    }

    if (line.find("CONNECT TIMEOUT") != std::string::npos) {
        Bluetooth_conn_state = ConnState::kIdle;
        Bluetooth_post_status(Lang::Strings::BT_CONNECT_TIMEOUT);
        return;
    }

    if (line.find("SETUP SCO") != std::string::npos) {
        Bluetooth_post_status(Lang::Strings::BT_CALL_MODE_SCO);
        return;
    }

    if (line.find("DISC SCO") != std::string::npos) {
        Bluetooth_post_status(Lang::Strings::BT_MUSIC_MODE_SCO);
        return;
    }

    Bluetooth_post_status(line.c_str());
}

void Bluetooth_on_uart_data(const std::vector<uint8_t>& data) {
    if (data.empty() || !Bluetooth_screen_active.load()) return;
    std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
    Bluetooth_rx_buffer.append(data.begin(), data.end());

    size_t pos = 0;
    while (true) {
        size_t nl = Bluetooth_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = Bluetooth_rx_buffer.substr(pos, nl - pos);
        Bluetooth_handle_response_line(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        Bluetooth_rx_buffer.erase(0, pos);
    }

    if (Bluetooth_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        Bluetooth_rx_buffer.clear();
    }
}

void Bluetooth_apply_mode_commands(BtMode mode) {
    SimpleUart& uart = SimpleUart::getInstance();

    Bluetooth_set_mode_audio_power(mode);
    switch (mode) {
        case BtMode::kMode1:
            Bluetooth_post_status(Lang::Strings::BT_SWITCH_MODE1);
            uart.sendString("AT+RX=2\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=2");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=1\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=1");
            break;
        case BtMode::kMode2:
            Bluetooth_post_status(Lang::Strings::BT_SWITCH_MODE2);
            uart.sendString("AT+TX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+TX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=2\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=2");
            break;
        case BtMode::kMode3:
            Bluetooth_post_status(Lang::Strings::BT_SWITCH_MODE3);
            uart.sendString("AT+RX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=3\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=3");
            break;
        case BtMode::kMode5:
            Bluetooth_post_status(Lang::Strings::BT_SWITCH_MODE5);
            if (!uart.sendString("AT+MODE=5\r\n")) {
                ESP_LOGE(TAG, "TX failed: AT+MODE=5");
            } else {
                ESP_LOGI(TAG, "TX: AT+MODE=5");
            }
            break;
        default:
            break;
    }
}

void Bluetooth_mode_cmd_task(void* param) {
    auto* args = static_cast<ModeCmdArgs*>(param);
    Bluetooth_apply_mode_commands(args->mode);
    delete args;
    Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void Bluetooth_send_mode_command(BtMode mode) {
    if (!SimpleUart::getInstance().isInitialized()) {
        Bluetooth_post_status(Lang::Strings::BT_UART_NOT_INIT);
        ESP_LOGE(TAG, "SimpleUart not initialized");
        return;
    }
    bool expected = false;
    if (!Bluetooth_mode_cmd_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "mode cmd busy, skip mode=%u", static_cast<unsigned>(mode));
        return;
    }
    auto* args = new (std::nothrow) ModeCmdArgs{mode};
    if (args == nullptr) {
        Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "mode cmd alloc failed");
        return;
    }
    if (xTaskCreate(Bluetooth_mode_cmd_task, "bt_mode_cmd", 4096, args, 5, nullptr) != pdPASS) {
        delete args;
        Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "mode cmd task create failed");
    }
}

void Bluetooth_call_mode_task(void* /*param*/) {
    {
        std::lock_guard<std::mutex> lock(audio_power_mutex);
        SetAudioPowerLocked(true);
    }
    SimpleUart& uart = SimpleUart::getInstance();
    Bluetooth_post_status(Lang::Strings::BT_SWITCH_CALL);
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+BTSCO=1\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=1");
    vTaskDelete(nullptr);
}

void Bluetooth_music_mode_task(void* /*param*/) {
    {
        std::lock_guard<std::mutex> lock(audio_power_mutex);
        if (!mode3_audio) SetAudioPowerLocked(false);
    }
    SimpleUart& uart = SimpleUart::getInstance();
    Bluetooth_post_status(Lang::Strings::BT_SWITCH_MUSIC);
    uart.sendString("AT+BTSCO=0\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=0");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    vTaskDelete(nullptr);
}

void Bluetooth_restore_mode_ui() {
    Bluetooth_refresh_mode_buttons();
    switch (Bluetooth_active_mode) {
        case BtMode::kMode1:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(Lang::Strings::BT_MODE1_SET);
            break;
        case BtMode::kMode2:
            Bluetooth_show_mode2_panel(true);
            Bluetooth_update_status_label(Lang::Strings::BT_MODE2_SET);
            break;
        case BtMode::kMode3:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(Lang::Strings::BT_MODE3_SET);
            break;
        case BtMode::kMode5:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(Lang::Strings::BT_MODE5_SET);
            break;
        default:
            Bluetooth_show_mode2_panel(false);
            break;
    }
}

void Bluetooth_reset_ui_state() {
    Bluetooth_State() = {};
    std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
    Bluetooth_rx_buffer.clear();
    Bluetooth_devices.clear();
    Bluetooth_conn_state = ConnState::kIdle;
}

lv_obj_t* Bluetooth_make_action_button(lv_obj_t* parent, const char* label, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, kSettingsOptionH);
    lv_obj_set_style_pad_hor(btn, kSettingsOptionPad, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}

lv_obj_t* Bluetooth_make_mode_button(lv_obj_t* parent, const char* label, int idx) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, 0);
    lv_obj_set_height(btn, kSettingsOptionH);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_pad_hor(btn, kSettingsOptionPad, 0);
    lv_obj_set_style_pad_ver(btn, UiSy(4), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, Bluetooth_on_mode_btn_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lbl);
    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}
