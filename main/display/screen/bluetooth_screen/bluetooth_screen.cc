#include "bluetooth_screen_priv.h"
#include "bluetooth_screen_ui.h"
#include "bluetooth_uart.h"

#include "assets/lang_config.h"
#include "assets/lang_bluetooth.h"
#include "application.h"
#include "lv_adapter_epdiy.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "power_hw.h"
#include "epd_refresh.h"
#include "settings_common.h"
#include "screen_common.h"
#include "simple_uart.hpp"
#include "ui_scale.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

std::atomic<bool> Bluetooth_mode_cmd_busy{false};
BluetoothUiState& Bluetooth_State() {
    static BluetoothUiState s;
    return s;
}
std::atomic<BtMode> Bluetooth_active_mode{BtMode::kNone};
std::atomic<ConnState> Bluetooth_conn_state{ConnState::kIdle};
std::mutex Bluetooth_rx_mutex;
std::string Bluetooth_rx_buffer;
std::vector<BtDevice> Bluetooth_devices;
std::atomic<bool> Bluetooth_screen_active{false};

namespace {
lv_timer_t* version_query_timer = nullptr; // UI 线程持有的版本查询超时计时器

void RestoreVersionButton() {
    if (version_query_timer != nullptr) {
        lv_timer_delete(version_query_timer);
        version_query_timer = nullptr;
    }
    auto* button = Bluetooth_State().check_version_btn;
    if (button != nullptr) {
        lv_label_set_text(lv_obj_get_child(button, 0), Lang::Strings::BT_CHECK_VERSION);
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    }
}

void QueryVersion() {
    auto* button = Bluetooth_State().check_version_btn;
    if (!Bluetooth_screen_active || button == nullptr || version_query_timer != nullptr) return;
    version_query_timer = lv_timer_create(
        [](lv_timer_t*) { RestoreVersionButton(); }, 5000, nullptr);
    if (version_query_timer == nullptr) {
        ESP_LOGE(TAG, "Cannot create version query timer");
        return;
    }
    lv_label_set_text(lv_obj_get_child(button, 0), Lang::Strings::BT_CHECKING_VERSION);
    lv_obj_add_state(button, LV_STATE_DISABLED);
    if (!Bluetooth_query_version()) RestoreVersionButton();
}

std::string last_connect_address; // UI 线程记录最近主动连接的耳机。
struct ResetArgs {
    BtMode mode;
    std::string address;
    uint32_t status_sequence;
};
}

void Bluetooth_strip_container(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void Bluetooth_update_status_label(const char* text) {
    if (Bluetooth_State().status_label == nullptr) {
        return;
    }
    lv_label_set_text(Bluetooth_State().status_label, text);
}

void Bluetooth_async_update_status(void* user_data) {
    auto* msg = static_cast<AsyncStatusMsg*>(user_data);
    Bluetooth_update_status_label(msg->text);
    delete msg;
}

void Bluetooth_post_status(const char* text) {
    if (!Bluetooth_screen_active) {
        return;
    }
    auto* msg = new (std::nothrow) AsyncStatusMsg{};
    if (msg == nullptr) return;
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    if (!ScreenLvAsync(Bluetooth_async_update_status, msg)) delete msg;
}

void Bluetooth_post_version(const char* version) {
    if (!Bluetooth_screen_active || version == nullptr) return;
    auto* message = new (std::nothrow) AsyncStatusMsg{};
    if (message == nullptr) return;
    std::snprintf(message->text, sizeof(message->text), "%s", version);
    if (!ScreenLvAsync([](void* context) {
        std::unique_ptr<AsyncStatusMsg> message(static_cast<AsyncStatusMsg*>(context));
        if (Bluetooth_screen_active && Bluetooth_State().version_label != nullptr) {
            RestoreVersionButton();
            lv_label_set_text_fmt(Bluetooth_State().version_label,
                                  Lang::Strings::BT_VERSION_FMT, message->text);
        }
    }, message)) delete message;
}

void Bluetooth_refresh_mode_buttons() {
    for (int i = 0; i < 3; ++i) {
        if (Bluetooth_State().mode_btns[i] == nullptr) {
            continue;
        }
        const bool active = (static_cast<int>(Bluetooth_active_mode.load()) == i + 1);
        SettingsStyleSelectable(Bluetooth_State().mode_btns[i], lv_obj_get_child(Bluetooth_State().mode_btns[i], 0), active);
    }
}

void Bluetooth_show_mode2_panel(bool show) {
    if (Bluetooth_State().mode2_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(Bluetooth_State().mode2_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(Bluetooth_State().mode2_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void Bluetooth_show_mode1_panel(bool show) {
    if (Bluetooth_State().mode1_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(Bluetooth_State().mode1_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(Bluetooth_State().mode1_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void Bluetooth_clear_device_list_ui() {
    if (Bluetooth_State().device_list == nullptr) {
        return;
    }
    lv_obj_clean(Bluetooth_State().device_list);
}

void Bluetooth_async_add_device_item(void* user_data) {
    auto* msg = static_cast<AsyncAddDeviceMsg*>(user_data);
    if (Bluetooth_State().device_list == nullptr) {
        delete msg;
        return;
    }

    lv_obj_t* item = lv_obj_create(Bluetooth_State().device_list);
    lv_obj_remove_style_all(item);
    lv_obj_set_width(item, lv_pct(100));
    lv_obj_set_height(item, kSettingsOptionH);
    lv_obj_set_style_pad_hor(item, kSettingsOptionPad, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(item);

    char* addr_copy = static_cast<char*>(lv_malloc(kAddrHexLen + 1));
    if (addr_copy != nullptr) {
        memcpy(addr_copy, msg->address, kAddrHexLen + 1);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                const char* addr = static_cast<const char*>(lv_event_get_user_data(e));
                if (addr == nullptr) {
                    return;
                }
                char cmd[48];
                if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return;
                last_connect_address = addr;
                snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s\r\n", addr);
                SimpleUart::getInstance().sendString(cmd);
                ESP_LOGI(TAG, "TX: AT+CONNECT=%s", addr);
                Bluetooth_conn_state = ConnState::kConnecting;
                char status[64];
                snprintf(status, sizeof(status), Lang::Strings::BT_CONNECTING_FMT, addr);
                Bluetooth_post_status(status);
            },
            LV_EVENT_CLICKED, addr_copy);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                char* addr = static_cast<char*>(lv_event_get_user_data(e));
                lv_free(addr);
            },
            LV_EVENT_DELETE, addr_copy);
    }

    lv_obj_t* lbl = lv_label_create(item);
    char display[96];
    if (msg->name[0] != '\0') {
        snprintf(display, sizeof(display), "%s\n%s", msg->name, msg->address);
    } else {
        snprintf(display, sizeof(display), "%s", msg->address);
    }
    lv_label_set_text(lbl, display);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    SettingsStyleSelectable(item, lbl, false);

    delete msg;
}

void Bluetooth_add_device_to_list(const char* address, const char* name) {
    if (!Bluetooth_screen_active) {
        return;
    }
    auto* msg = new (std::nothrow) AsyncAddDeviceMsg{};
    if (msg == nullptr) return;
    snprintf(msg->address, sizeof(msg->address), "%s", address);
    snprintf(msg->name, sizeof(msg->name), "%s", name);
    if (!ScreenLvAsync(Bluetooth_async_add_device_item, msg)) delete msg;
}

void Bluetooth_async_clear_list(void* /*user_data*/) {
    Bluetooth_clear_device_list_ui();
}

void Bluetooth_post_clear_list() {
    if (!Bluetooth_screen_active) {
        return;
    }
    ScreenLvAsync(Bluetooth_async_clear_list);
}

void Bluetooth_async_on_mode1_set(void* /*user_data*/) {
    Bluetooth_refresh_mode_buttons();
    Bluetooth_show_mode2_panel(false);
    Bluetooth_show_mode1_panel(true);
}

void Bluetooth_async_on_mode2_set(void* /*user_data*/) {
    Bluetooth_refresh_mode_buttons();
    Bluetooth_show_mode1_panel(false);
    Bluetooth_show_mode2_panel(true);
}

void Bluetooth_async_on_mode3_set(void* /*user_data*/) {
    Bluetooth_refresh_mode_buttons();
    Bluetooth_show_mode1_panel(false);
    Bluetooth_show_mode2_panel(false);
}

void Bluetooth_on_mode_btn_clicked(lv_event_t* e) {
    const int idx =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    Bluetooth_send_mode_command(static_cast<BtMode>(idx + 1));
}

void Bluetooth_bt_reset_task(void* param) {
    auto* args = static_cast<ResetArgs*>(param);
    // 保持唤醒和网络电源策略，但不申请会开启 PA 的 BtAudio。
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    PowerPolicy::GetInstance().Acquire(PowerNeed::PeripheralReset);
    // 先让“正在复位”真正落墨，再进入禁止屏幕操作的断电窗口。
    if (!EpdWait(args->status_sequence, 5000)) {
        Bluetooth_post_status(Lang::Strings::BT_PWR_RESET_FAILED);
        PowerPolicy::GetInstance().Release(PowerNeed::PeripheralReset);
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
        delete args;
        vTaskDelete(nullptr);
        return;
    }
    Bluetooth_active_mode = BtMode::kNone;
    Bluetooth_set_mode_audio_power(BtMode::kNone);
    Bluetooth_conn_state = ConnState::kIdle;
    esp_err_t err = power_hw_bt_power_reset();
    if (err == ESP_OK && SimpleUart::getInstance().isInitialized()) {
        Bluetooth_post_status(Lang::Strings::BT_PWR_RESET_RECONNECT);
        // 给音频芯片启动时间，再恢复复位前模式和主动连接的耳机。
        vTaskDelay(pdMS_TO_TICKS(500));
        Bluetooth_apply_mode_commands(args->mode);
        Bluetooth_active_mode = args->mode;
        if (args->mode == BtMode::kMode2 && !args->address.empty()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            SimpleUart::getInstance().sendString("AT+CONNECT=" + args->address + "\r\n");
            Bluetooth_conn_state = ConnState::kConnecting;
        }
        if (Bluetooth_screen_active) {
            ScreenLvAsync([](void*) { Bluetooth_restore_mode_ui(); }, nullptr);
        }
    }
    PowerPolicy::GetInstance().Release(PowerNeed::PeripheralReset);
    PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
    if (err == ESP_OK) {
        Bluetooth_post_status(args->mode == BtMode::kMode2 && !args->address.empty()
                                  ? Lang::Strings::BT_PWR_RESET_RECONNECT
                                  : Lang::Strings::BT_PWR_RESET_OK);
    } else {
        ESP_LOGE(TAG, "BT power reset failed: %s", esp_err_to_name(err));
        Bluetooth_post_status(Lang::Strings::BT_PWR_RESET_FAILED);
    }
    delete args;
    Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void Bluetooth_on_reset_bt_clicked(lv_event_t* /*e*/) {
    bool expected = false;
    if (!Bluetooth_mode_cmd_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    auto* args = new (std::nothrow) ResetArgs{
        Bluetooth_active_mode == BtMode::kNone ? BtMode::kMode1 : Bluetooth_active_mode.load(),
        last_connect_address, EpdGetStats().submitted + 1};
    Bluetooth_update_status_label(Lang::Strings::BT_PWR_RESETTING);
    if (args == nullptr || xTaskCreate(Bluetooth_bt_reset_task, "bt_reset", 4096, args, 5, nullptr) != pdPASS) {
        delete args;
        Bluetooth_mode_cmd_busy.store(false, std::memory_order_release);
        Bluetooth_update_status_label(Lang::Strings::BT_PWR_RESET_FAILED);
        ESP_LOGE(TAG, "BT reset task create failed");
    }
}

void Bluetooth_on_scan_clicked(lv_event_t* /*e*/) {
    if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return;
    if (Bluetooth_active_mode != BtMode::kMode2) {
        Bluetooth_post_status(Lang::Strings::BT_NEED_MODE2);
        return;
    }
    if (!SimpleUart::getInstance().isInitialized()) {
        Bluetooth_post_status(Lang::Strings::BT_UART_NOT_INIT);
        return;
    }
    SimpleUart::getInstance().sendString("AT+INQUIRING\r\n");
    ESP_LOGI(TAG, "TX: AT+INQUIRING");
    {
        std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
        Bluetooth_devices.clear();
    }
    Bluetooth_post_clear_list();
    Bluetooth_post_status(Lang::Strings::BT_SCAN_START);
}

void Bluetooth_on_call_mode_clicked(lv_event_t* /*e*/) {
    if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return;
    if (Bluetooth_conn_state != ConnState::kConnected) {
        Bluetooth_post_status(Lang::Strings::BT_NEED_CONNECT);
        return;
    }
    xTaskCreate(Bluetooth_call_mode_task, "bt_call_mode", 4096, nullptr, 5, nullptr);
}

void Bluetooth_on_music_mode_clicked(lv_event_t* /*e*/) {
    if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return;
    if (Bluetooth_conn_state != ConnState::kConnected) {
        Bluetooth_post_status(Lang::Strings::BT_NEED_CONNECT);
        return;
    }
    xTaskCreate(Bluetooth_music_mode_task, "bt_music_mode", 4096, nullptr, 5, nullptr);
}

void BluetoothScreen::BuildInto(lv_obj_t* parent) {
    Bluetooth_conn_state = ConnState::kIdle;
    {
        std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
        Bluetooth_rx_buffer.clear();
        Bluetooth_devices.clear();
    }
    Bluetooth_State().root = parent;

    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, Lang::Strings::BT_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    auto add_divider = [parent]() {
        lv_obj_t* divider = lv_obj_create(parent);
        Bluetooth_strip_container(divider);
        lv_obj_set_size(divider, lv_pct(100), kSettingsBorderW);
        lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
    };

    lv_obj_t* version = lv_label_create(parent);
    Bluetooth_State().version_label = version;
    lv_label_set_text_fmt(version, Lang::Strings::BT_VERSION_FMT, Lang::Strings::COMMON_UNKNOWN);
    lv_obj_set_width(version, lv_pct(100));
    lv_label_set_long_mode(version, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(version, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(version, lv_color_black(), 0);
    lv_obj_clear_flag(version, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* check_version = Bluetooth_make_action_button(
        parent, Lang::Strings::BT_CHECK_VERSION,
        [](lv_event_t* /*e*/) { QueryVersion(); });
    Bluetooth_State().check_version_btn = check_version;
    lv_obj_set_width(check_version, LV_SIZE_CONTENT);
    add_divider();

    lv_obj_t* reset_row = lv_obj_create(parent);
    Bluetooth_strip_container(reset_row);
    lv_obj_set_width(reset_row, lv_pct(100));
    lv_obj_set_height(reset_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(reset_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(reset_row, kSettingsOptionGap, 0);

    lv_obj_t* reset_hint = lv_label_create(reset_row);
    lv_label_set_text(reset_hint, Lang::Strings::BT_RESET_HINT);
    lv_obj_set_style_text_font(reset_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(reset_hint, lv_color_black(), 0);
    lv_obj_set_width(reset_hint, lv_pct(100));
    lv_label_set_long_mode(reset_hint, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(reset_hint, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* reset = Bluetooth_make_action_button(reset_row, Lang::Strings::BT_RESET_BTN, Bluetooth_on_reset_bt_clicked);
    lv_obj_set_width(reset, UiSx(120));
    add_divider();

    lv_obj_t* mode_title = lv_label_create(parent);
    lv_label_set_text(mode_title, Lang::Strings::BT_MODE_TITLE);
    lv_obj_set_style_text_font(mode_title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(mode_title, lv_color_black(), 0);
    lv_obj_clear_flag(mode_title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* mode_row = lv_obj_create(parent);
    Bluetooth_strip_container(mode_row);
    lv_obj_set_width(mode_row, lv_pct(100));
    lv_obj_set_height(mode_row, kSettingsOptionH);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mode_row, kSettingsOptionGap, 0);

    const char* mode_labels[] = {Lang::Strings::BT_MODE1, Lang::Strings::BT_MODE2, Lang::Strings::BT_MODE3};
    for (int i = 0; i < 3; ++i) {
        Bluetooth_State().mode_btns[i] = Bluetooth_make_mode_button(mode_row, mode_labels[i], i);
    }

    lv_obj_t* m1_panel = lv_obj_create(parent);
    Bluetooth_State().mode1_panel = m1_panel;
    Bluetooth_strip_container(m1_panel);
    lv_obj_set_width(m1_panel, lv_pct(100));
    lv_obj_set_height(m1_panel, UiSy(80));
    lv_obj_set_style_border_width(m1_panel, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(m1_panel, lv_color_black(), 0);
    lv_obj_set_style_radius(m1_panel, kSettingsOptionRadius, 0);
    lv_obj_add_flag(m1_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* m1_hint = lv_label_create(m1_panel);
    lv_label_set_text(m1_hint, Lang::Strings::BT_MODE1_ACTIVE);
    lv_obj_set_style_text_font(m1_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(m1_hint, lv_color_black(), 0);
    lv_obj_center(m1_hint);
    lv_obj_clear_flag(m1_hint, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* panel = lv_obj_create(parent);
    Bluetooth_State().mode2_panel = panel;
    Bluetooth_strip_container(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, UiSy(280));
    lv_obj_set_style_border_width(panel, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_radius(panel, kSettingsOptionRadius, 0);
    lv_obj_set_style_pad_all(panel, UiSx(8), 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scan = Bluetooth_make_action_button(panel, Lang::Strings::BT_SCAN_BTN, Bluetooth_on_scan_clicked);
    Bluetooth_State().scan_btn = scan;
    lv_obj_set_width(scan, lv_pct(100));
    lv_obj_align(scan, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* list = lv_obj_create(panel);
    Bluetooth_State().device_list = list;
    Bluetooth_strip_container(list);
    lv_obj_set_size(list, lv_pct(100), UiSy(140));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, kSettingsOptionH + UiSy(8));
    lv_obj_set_style_border_width(list, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(list, lv_color_black(), 0);
    lv_obj_set_style_radius(list, kSettingsOptionRadius, 0);
    lv_obj_set_style_pad_all(list, UiSx(6), 0);
    lv_obj_set_style_pad_row(list, UiSy(6), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t* btn_row = lv_obj_create(panel);
    Bluetooth_strip_container(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, kSettingsOptionH);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, kSettingsOptionGap, 0);

    lv_obj_t* music = Bluetooth_make_action_button(btn_row, Lang::Strings::BT_MUSIC_BTN, Bluetooth_on_music_mode_clicked);
    Bluetooth_State().music_btn = music;
    lv_obj_set_flex_grow(music, 1);

    lv_obj_t* call = Bluetooth_make_action_button(btn_row, Lang::Strings::BT_CALL_BTN, Bluetooth_on_call_mode_clicked);
    Bluetooth_State().call_btn = call;
    lv_obj_set_flex_grow(call, 1);

    lv_obj_t* status = lv_label_create(parent);
    Bluetooth_State().status_label = status;
    lv_label_set_text(status, Lang::Strings::BT_SELECT_MODE);
    lv_obj_set_style_text_font(status, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(status, lv_color_black(), 0);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);

    // 开机 MetalioAudio 已下发模式1；UI 首次进入时对齐选中态
    if (Bluetooth_active_mode == BtMode::kNone) {
        Bluetooth_active_mode = BtMode::kMode1;
    }
    Bluetooth_restore_mode_ui();
}

void BluetoothScreen::ResetUi() {
    RestoreVersionButton();
    Bluetooth_reset_ui_state();
}

void BluetoothScreen::ApplyDefaultMode() {
    Bluetooth_set_mode_audio_power(BtMode::kMode1);
    Bluetooth_active_mode = BtMode::kMode1;
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "ApplyDefaultMode: UART not initialized");
        return;
    }
    uart.sendString("AT+RX=2\r\n");
    ESP_LOGI(TAG, "TX: AT+RX=2");
    vTaskDelay(pdMS_TO_TICKS(700));
    uart.sendString("AT+MODE=1\r\n");
    ESP_LOGI(TAG, "TX: AT+MODE=1");
    // 勿在此拉 PA：MAIN_PWR 恢复也会走本函数，进百问会 PA on→off。
    // PA 仅由 PowerPolicy（Speaking / BtAudio / 电话）按需开关。
}

void BluetoothScreen::OnActivated() {
    ESP_LOGI(TAG, "activate: bluetooth tab");
    Bluetooth_screen_active = true;
    {
        std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
        Bluetooth_rx_buffer.clear();
    }
    auto& uart = SimpleUart::getInstance();
    if (Bluetooth_State().version_label != nullptr) {
        lv_label_set_text_fmt(Bluetooth_State().version_label, Lang::Strings::BT_VERSION_FMT,
                              Lang::Strings::COMMON_UNKNOWN);
    }
    uart.registerCallback(Bluetooth_on_uart_data);
    QueryVersion();
}

void BluetoothScreen::OnDeactivated() {
    ESP_LOGI(TAG, "deactivate: bluetooth tab");
    RestoreVersionButton();
    Bluetooth_release_page_audio_power();
    SimpleUart::getInstance().registerCallback(
        std::function<void(const std::vector<uint8_t>&)>());
    Bluetooth_screen_active = false;
}
