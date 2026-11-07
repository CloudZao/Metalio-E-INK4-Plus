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
#include "settings_screen.h"
#include "vk_key_handler.h"
#include "screen_common.h"
#include "screen_text.h"
#include "screen_keyboard.h"
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
lv_obj_t* CreateHidTestScreen();
lv_obj_t* name_keyboard_panel = nullptr; // 名称键盘遮罩。
lv_obj_t* name_confirm_panel = nullptr; // 名称确认遮罩。
lv_obj_t* name_hint = nullptr; // 键盘内提示。
lv_obj_t* name_input = nullptr; // 静态输入文本。
lv_obj_t* name_box = nullptr; // 输入框。
lv_obj_t* name_keyboard = nullptr; // 与 Wi-Fi 共用的键盘。
char edited_name[64] = {}; // 当前编辑内容。
char pending_name[64] = {}; // 勾选时冻结的待确认名称。

void CloseNameConfirmation() {
    if (name_confirm_panel != nullptr) lv_obj_delete(name_confirm_panel);
}

void CloseNameKeyboard() {
    CloseNameConfirmation();
    if (name_keyboard_panel != nullptr) lv_obj_delete(name_keyboard_panel);
}

void RefreshNameInput() {
    if (name_input == nullptr) return;
    const char* tail = edited_name;
    const auto* font = fontpack_lv_font_ui();
    if (font == nullptr) font = LV_FONT_DEFAULT;
    const int width = lv_obj_get_content_width(name_box) - UiSx(12);
    lv_point_t size;
    while (true) {
        lv_text_get_size(&size, tail, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (width <= 0 || size.x <= width || *tail == '\0') break;
        do { ++tail; } while ((static_cast<unsigned char>(*tail) & 0xC0) == 0x80);
    }
    lv_label_set_text(name_input, tail);
    lv_obj_set_width(name_input, size.x > 0 ? size.x : 1);
}

void ShowNameConfirmation() {
    if (name_confirm_panel != nullptr) return;
    if (!Bluetooth_is_valid_name(edited_name)) {
        lv_label_set_text(name_hint, Lang::Strings::BT_NAME_INVALID);
        return;
    }
    snprintf(pending_name, sizeof(pending_name), "%s", edited_name);
    ScreenKeyboardPause(name_keyboard);
    auto* mask = lv_obj_create(lv_screen_active());
    name_confirm_panel = mask;
    Bluetooth_strip_container(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, [](lv_event_t* e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) CloseNameConfirmation();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { name_confirm_panel = nullptr; }, LV_EVENT_DELETE, nullptr);
    auto* card = lv_obj_create(mask);
    Bluetooth_strip_container(card);
    lv_obj_set_width(card, LV_HOR_RES - UiSx(80));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UiSx(20), 0);
    lv_obj_set_style_pad_row(card, UiSy(20), 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(card, nullptr, 0);
    lv_obj_set_style_border_width(card, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, kSettingsOptionRadius, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    auto* message = lv_label_create(card);
    lv_label_set_text_fmt(message, Lang::Strings::BT_NAME_CONFIRM_FMT, pending_name);
    lv_obj_set_width(message, lv_pct(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(message, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(message, lv_color_black(), 0);
    lv_obj_clear_flag(message, LV_OBJ_FLAG_CLICKABLE);
    auto* row = lv_obj_create(card);
    Bluetooth_strip_container(row);
    lv_obj_set_size(row, lv_pct(100), kSettingsOptionH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, kSettingsOptionGap, 0);
    auto* cancel = Bluetooth_make_action_button(row, Lang::Strings::COMMON_CANCEL,
        [](lv_event_t*) { CloseNameConfirmation(); });
    auto* confirm = Bluetooth_make_action_button(row, Lang::Strings::BT_NAME_CONFIRM,
        [](lv_event_t*) {
            if (Bluetooth_rename(pending_name)) CloseNameKeyboard();
            else CloseNameConfirmation();
        });
    for (auto* button : {cancel, confirm}) {
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED,
                            lv_obj_get_child(button, 0));
    }
}

[[maybe_unused]] void OpenNameKeyboard() {
    if (name_keyboard_panel != nullptr || Bluetooth_mode_cmd_busy.load()) return;
    auto* mask = lv_obj_create(lv_screen_active());
    name_keyboard_panel = mask;
    Bluetooth_strip_container(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, [](lv_event_t* e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) CloseNameKeyboard();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(mask, [](lv_event_t*) {
        name_keyboard_panel = nullptr;
        name_input = name_hint = name_box = name_keyboard = nullptr;
        edited_name[0] = pending_name[0] = '\0';
    }, LV_EVENT_DELETE, nullptr);
    auto* panel = lv_obj_create(mask);
    Bluetooth_strip_container(panel);
    lv_obj_set_size(panel, lv_pct(100), UiSx(260) + UiSy(180) + kSettingsContentPad * 2);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, kSettingsBorderW, 0);
    lv_obj_set_style_pad_all(panel, kSettingsContentPad, 0);
    lv_obj_set_style_pad_row(panel, UiSy(8), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, [](lv_event_t* e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) CloseNameKeyboard();
    }, LV_EVENT_CLICKED, nullptr);
    name_hint = lv_label_create(panel);
    lv_label_set_text(name_hint, Lang::Strings::BT_NAME_EDIT_HINT);
    lv_obj_set_width(name_hint, lv_pct(100));
    lv_label_set_long_mode(name_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(name_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(name_hint, lv_color_black(), 0);
    lv_obj_clear_flag(name_hint, LV_OBJ_FLAG_CLICKABLE);
    name_box = lv_obj_create(panel);
    Bluetooth_strip_container(name_box);
    lv_obj_set_size(name_box, lv_pct(100), UiSy(64));
    lv_obj_set_style_border_width(name_box, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(name_box, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(name_box, UiSx(10), 0);
    lv_obj_set_flex_flow(name_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(name_box, LV_OBJ_FLAG_CLICKABLE);
    name_input = lv_label_create(name_box);
    lv_label_set_long_mode(name_input, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name_input, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(name_input, lv_color_black(), 0);
    lv_obj_clear_flag(name_input, LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_precise_t cursor_points[] = {
        {0, 0}, {UiSx(8), 0}, {UiSx(4), 0}, {UiSx(4), UiSy(38)}, {0, UiSy(38)}, {UiSx(8), UiSy(38)}};
    auto* cursor = lv_line_create(name_box);
    lv_line_set_points(cursor, cursor_points, 6);
    lv_obj_set_style_line_width(cursor, UiSx(2), 0);
    lv_obj_set_style_line_color(cursor, lv_color_black(), 0);
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
    auto* label = Bluetooth_State().name_label;
    edited_name[0] = '\0';
    if (label != nullptr && strcmp(lv_label_get_text(label), Lang::Strings::COMMON_UNKNOWN) != 0) {
        snprintf(edited_name, sizeof(edited_name), "%s", lv_label_get_text(label));
    }
    name_keyboard = ScreenKeyboardCreate(panel, [](ScreenKeyboardEvent event, const char* text, void*) {
        size_t length = strlen(edited_name);
        switch (event) {
            case ScreenKeyboardEvent::kInsert:
                if (length + strlen(text) < sizeof(edited_name)) strcat(edited_name, text);
                break;
            case ScreenKeyboardEvent::kBackspace:
                if (length > 0) {
                    do { --length; } while (length > 0 && (static_cast<unsigned char>(edited_name[length]) & 0xC0) == 0x80);
                    edited_name[length] = '\0';
                }
                break;
            case ScreenKeyboardEvent::kClear: edited_name[0] = '\0'; break;
            case ScreenKeyboardEvent::kConfirm: ShowNameConfirmation(); return;
            case ScreenKeyboardEvent::kLayout: break;
        }
        RefreshNameInput();
    });
    if (name_keyboard == nullptr) { CloseNameKeyboard(); return; }
    lv_obj_set_height(name_keyboard, 0);
    lv_obj_set_flex_grow(name_keyboard, 1);
    ScreenKeyboardReset(name_keyboard);
    lv_obj_update_layout(panel);
    RefreshNameInput();
}

lv_obj_t* hid_test_screen = nullptr; // HID 全屏测试页。
lv_obj_t* hid_test_status = nullptr; // HID 测试页独立状态栏。
bool hid_alt_held = false; // UI 线程记录尚未释放的 Alt。
lv_obj_t* audio_devices_page = nullptr; // 全屏音频设备管理页。
lv_obj_t* audio_devices_status = nullptr; // 管理页状态文案。

void CloseAudioDevices() {
    if (audio_devices_page == nullptr) return;
    auto* panel = Bluetooth_State().mode2_panel;
    if (panel != nullptr && Bluetooth_State().root != nullptr) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(panel, Bluetooth_State().root);
        lv_obj_set_flex_grow(panel, 0);
        lv_obj_set_height(panel, UiSy(280));
    }
    lv_obj_delete(audio_devices_page);
}

void OpenAudioDevices() {
    auto* panel = Bluetooth_State().mode2_panel;
    if (audio_devices_page != nullptr || panel == nullptr ||
        Bluetooth_active_mode != BtMode::kMode2 || Bluetooth_mode_cmd_busy.load()) return;
    auto* page = lv_obj_create(lv_screen_active());
    audio_devices_page = page;
    Bluetooth_strip_container(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES - ScreenStatusBarHeight());
    lv_obj_align(page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, kSettingsContentPad, 0);
    lv_obj_set_style_pad_row(page, kSettingsOptionGap, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(page, [](lv_event_t*) {
        audio_devices_page = audio_devices_status = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    auto* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::BT_AUDIO_DEVICES);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_parent(panel, page);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(panel, 0);
    lv_obj_set_flex_grow(panel, 1);
    audio_devices_status = lv_label_create(page);
    lv_obj_set_size(audio_devices_status, lv_pct(100), UiSy(80));
    lv_label_set_long_mode(audio_devices_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(audio_devices_status, Lang::Strings::BT_MODE2_SET);
    lv_obj_set_style_text_font(audio_devices_status, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(audio_devices_status, lv_color_black(), 0);
    lv_obj_clear_flag(audio_devices_status, LV_OBJ_FLAG_CLICKABLE);
}

bool ReleaseHidAlt() {
    if (!hid_alt_held) return true;
    if (!Bluetooth_send_hid_shortcut("ALT,UP")) return false;
    hid_alt_held = false;
    return true;
}

bool OnHidTestNavigationKey(const char* key) {
    if (key == nullptr || strcmp(key, "vk_home") != 0) return false;
    if (ReleaseHidAlt()) ScreenNavigateBack();
    return true;
}

void OnHidAltEvent(lv_event_t* e) {
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (!hid_alt_held && Bluetooth_screen_active) {
            hid_alt_held = Bluetooth_send_hid_shortcut("ALT,DOWN");
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
        ReleaseHidAlt();
    }
}

void RestoreVersionButton() {
    if (version_query_timer != nullptr) {
        lv_timer_delete(version_query_timer);
        version_query_timer = nullptr;
    }
    auto* button = Bluetooth_State().check_version_btn;
    if (button != nullptr) {
        lv_label_set_text(Bluetooth_State().version_hint_label, Lang::Strings::BT_CHECK_VERSION);
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    }
}

void QueryVersion() {
    auto* button = Bluetooth_State().check_version_btn;
    if (!Bluetooth_screen_active || button == nullptr || version_query_timer != nullptr ||
        Bluetooth_mode_cmd_busy.load()) return;
    version_query_timer = lv_timer_create(
        [](lv_timer_t*) { RestoreVersionButton(); }, 5000, nullptr);
    if (version_query_timer == nullptr) {
        ESP_LOGE(TAG, "Cannot create version query timer");
        return;
    }
    lv_label_set_text(Bluetooth_State().version_hint_label, Lang::Strings::BT_CHECKING_VERSION);
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
    if (audio_devices_status != nullptr) {
        lv_label_set_text(audio_devices_status, text);
        return;
    }
    if (name_hint != nullptr) {
        lv_label_set_text(name_hint, text);
        return;
    }
    if (hid_test_status != nullptr && lv_screen_active() == hid_test_screen) {
        lv_label_set_text(hid_test_status, text);
        return;
    }
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
            lv_label_set_text(Bluetooth_State().version_label, message->text);
        }
    }, message)) delete message;
}

void Bluetooth_post_name(const char* name) {
    auto* message = new (std::nothrow) AsyncStatusMsg{};
    if (message == nullptr) return;
    snprintf(message->text, sizeof(message->text), "%s", name);
    if (!ScreenLvAsync([](void* p) {
        std::unique_ptr<AsyncStatusMsg> msg(static_cast<AsyncStatusMsg*>(p));
        if (Bluetooth_screen_active && Bluetooth_State().name_label != nullptr) {
            lv_label_set_text(Bluetooth_State().name_label, msg->text);
        }
    }, message)) delete message;
}

void Bluetooth_refresh_mode_buttons() {
    if (auto* label = Bluetooth_State().current_mode_label) {
        const char* description = Lang::Strings::COMMON_UNKNOWN;
        switch (Bluetooth_active_mode.load()) {
            case BtMode::kMode1: description = Lang::Strings::BT_MODE1; break;
            case BtMode::kMode2: description = Lang::Strings::BT_MODE2; break;
            case BtMode::kMode3: description = Lang::Strings::BT_MODE3; break;
            case BtMode::kMode5: description = Lang::Strings::BT_MODE5; break;
            default: break;
        }
        lv_label_set_text(label, description);
    }
    if (auto* label = Bluetooth_State().mode_description_label) {
        const char* description = Lang::Strings::COMMON_UNKNOWN;
        switch (Bluetooth_active_mode.load()) {
            case BtMode::kMode1: description = Lang::Strings::BT_MODE1_DESC; break;
            case BtMode::kMode2: description = Lang::Strings::BT_MODE2_DESC; break;
            case BtMode::kMode3: description = Lang::Strings::BT_MODE3_DESC; break;
            case BtMode::kMode5: description = Lang::Strings::BT_MODE5_DESC; break;
            default: break;
        }
        lv_label_set_text(label, description);
        auto* area = lv_obj_get_parent(label);
        ScreenFitLabel(label, lv_obj_get_content_width(area), lv_obj_get_content_height(area));
        lv_obj_center(label);
    }
    if (Bluetooth_active_mode != BtMode::kMode5) ReleaseHidAlt();
    auto* hid_panel = Bluetooth_State().hid_panel;
    if (hid_panel != nullptr) {
        if (Bluetooth_active_mode == BtMode::kMode5) {
            lv_obj_clear_flag(hid_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hid_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < kBluetoothModeCount; ++i) {
        if (Bluetooth_State().mode_btns[i] == nullptr) {
            continue;
        }
        const bool active = (Bluetooth_active_mode.load() == kBluetoothModes[i]);
        SettingsStyleSelectable(Bluetooth_State().mode_btns[i], lv_obj_get_child(Bluetooth_State().mode_btns[i], 0), active);
    }
}

void Bluetooth_show_mode2_panel(bool show) {
    auto* entry = Bluetooth_State().mode2_entry;
    if (entry != nullptr) {
        if (show) lv_obj_clear_flag(entry, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(entry, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show) CloseAudioDevices();
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

}

void Bluetooth_async_on_mode2_set(void* /*user_data*/) {
    Bluetooth_refresh_mode_buttons();

    Bluetooth_show_mode2_panel(true);
}

void Bluetooth_async_on_mode3_set(void* /*user_data*/) {
    Bluetooth_refresh_mode_buttons();

    Bluetooth_show_mode2_panel(false);
}

void Bluetooth_on_mode_btn_clicked(lv_event_t* e) {
    const int idx =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= kBluetoothModeCount) return;
    if (!ReleaseHidAlt()) return;
    Bluetooth_send_mode_command(kBluetoothModes[idx]);
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
    if (!ReleaseHidAlt()) return;
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

namespace {
lv_obj_t* CreateHidTestScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    hid_test_screen = screen;
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    const auto status_bar = ScreenCreateStatusBar(screen);

    lv_obj_t* page = lv_obj_create(screen);
    Bluetooth_strip_container(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES - status_bar.height);
    lv_obj_align(page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(page, kSettingsContentPad, 0);
    lv_obj_set_style_pad_row(page, kSettingsOptionGap, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    for (const char* text : {Lang::Strings::BT_HID_TITLE, Lang::Strings::BT_HID_HINT}) {
        lv_obj_t* label = lv_label_create(page);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        if (text == Lang::Strings::BT_HID_TITLE) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t* keys = lv_obj_create(page);
    Bluetooth_strip_container(keys);
    lv_obj_set_width(keys, lv_pct(100));
    lv_obj_set_height(keys, 0);
    lv_obj_set_flex_grow(keys, 1);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(keys, kSettingsOptionGap, 0);
    lv_obj_add_flag(keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(keys, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(keys, LV_SCROLLBAR_MODE_AUTO);
    const char* labels[] = {"A", "B", "1", Lang::Strings::BT_HID_ENTER,
        Lang::Strings::BT_HID_SPACE, Lang::Strings::BT_HID_TAB, Lang::Strings::BT_HID_ESC,
        Lang::Strings::BT_HID_BACKSPACE, Lang::Strings::BT_HID_DELETE,
        Lang::Strings::BT_HID_LEFT, Lang::Strings::BT_HID_RIGHT, Lang::Strings::BT_HID_UP,
        Lang::Strings::BT_HID_DOWN, "Ctrl+A", "Ctrl+C", "Ctrl+V", "Alt+Tab", "Win+D"};
    static const char* const shortcuts[] = {"A", "B", "1", "ENTER", "SPACE", "TAB", "ESC",
        "BACKSPACE", "DELETE", "LEFT", "RIGHT", "UP", "DOWN", "CTRL+A", "CTRL+C", "CTRL+V",
        "ALT+TAB", "WIN+D"};
    for (int i = 0; i < 18; i += 3) {
        lv_obj_t* row = lv_obj_create(keys);
        Bluetooth_strip_container(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kSettingsOptionH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, kSettingsOptionGap, 0);
        for (int j = i; j < i + 3; ++j) {
            lv_obj_t* button = Bluetooth_make_action_button(row, labels[j], [](lv_event_t* e) {
                const char* shortcut = static_cast<const char*>(
                    lv_obj_get_user_data(lv_event_get_target_obj(e)));
                Bluetooth_send_hid_shortcut(shortcut);
            });
            lv_obj_set_user_data(button, const_cast<char*>(shortcuts[j]));
            lv_obj_set_width(button, 0);
            lv_obj_set_flex_grow(button, 1);
            lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED,
                                lv_obj_get_child(button, 0));
        }
    }
    lv_obj_t* alt_button = Bluetooth_make_action_button(page, Lang::Strings::BT_HID_HOLD_ALT,
                                                       OnHidAltEvent);
    lv_obj_set_width(alt_button, lv_pct(100));
    for (auto code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_DELETE}) {
        lv_obj_add_event_cb(alt_button, OnHidAltEvent, code, nullptr);
    }
    lv_obj_t* release = Bluetooth_make_action_button(page, Lang::Strings::BT_HID_RELEASE,
        [](lv_event_t*) {
            if (Bluetooth_send_hid_shortcut("RELEASE")) hid_alt_held = false;
        });
    lv_obj_set_width(release, lv_pct(100));
    hid_test_status = lv_label_create(page);
    lv_obj_set_width(hid_test_status, lv_pct(100));
    lv_obj_set_height(hid_test_status, UiSy(88));
    lv_label_set_long_mode(hid_test_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hid_test_status, Lang::Strings::BT_HID_READY);
    lv_obj_clear_flag(hid_test_status, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        const auto code = lv_event_get_code(e);
        if (code == LV_EVENT_SCREEN_LOADED) {
            Bluetooth_screen_active = true;
            SimpleUart::getInstance().registerCallback(Bluetooth_on_uart_data);
        } else if (code == LV_EVENT_SCREEN_UNLOADED) {
            ReleaseHidAlt();
            hid_test_status = nullptr;
            auto* root = Bluetooth_State().root;
            // 返回设置页时，新页已接管 UART，不能由旧测试页注销。
            if (root == nullptr || lv_obj_get_screen(root) != lv_screen_active()) {
                Bluetooth_screen_active = false;
                SimpleUart::getInstance().registerCallback({});
            }
        } else if (code == LV_EVENT_DELETE) {
            if (lv_event_get_target_obj(e) == hid_test_screen) hid_test_screen = nullptr;
        }
    }, LV_EVENT_ALL, nullptr);
    ScreenSetIsHome(false);
    VkKeyScreenDesc navigation{CreateHidTestScreen};
    navigation.on_key = OnHidTestNavigationKey;
    navigation.on_key_long_press = OnHidTestNavigationKey;
    VkKey_AttachScreen(screen, "bluetooth_hid_test", navigation);
    return screen;
}
} // namespace

void BluetoothScreen::BuildInto(lv_obj_t* parent) {
    Bluetooth_conn_state = ConnState::kIdle;
    {
        std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
        Bluetooth_rx_buffer.clear();
        Bluetooth_devices.clear();
    }
    Bluetooth_State().root = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_row(parent, UiSy(6), 0);
    lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);

    auto info_row = [parent](const char* title, const char* value, const char* action,
                             lv_event_cb_t callback, lv_obj_t** button) {
        auto* row = lv_obj_create(parent);
        Bluetooth_strip_container(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, kSettingsInfoRowH, 0);
        lv_obj_set_style_pad_ver(row, UiSy(4), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, kSettingsBorderW, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, kSettingsOptionGap, 0);
        auto* text = lv_obj_create(row);
        Bluetooth_strip_container(text);
        lv_obj_set_width(text, 0);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, UiSy(4), 0);
        lv_obj_t* value_label = nullptr;
        for (const char* content : {title, value}) {
            auto* label = lv_label_create(text);
            lv_label_set_text(label, content);
            lv_obj_set_width(label, lv_pct(100));
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
            lv_obj_set_style_text_color(label, lv_color_black(), 0);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
            value_label = label;
        }
        if (action != nullptr) {
            auto* hint = lv_label_create(text);
            lv_label_set_text(hint, action);
            lv_obj_set_width(hint, lv_pct(100));
            lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
            lv_obj_set_style_text_color(hint, lv_color_black(), 0);
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
            if (button != nullptr) {
                *button = row;
                Bluetooth_State().version_hint_label = hint;
            }
        }
        lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
        if (callback != nullptr) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(row);
            lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, nullptr);
        } else lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        return value_label;
    };
    Bluetooth_State().name_label = info_row(Lang::Strings::BT_NAME_TITLE,
        Lang::Strings::COMMON_UNKNOWN, nullptr, nullptr, nullptr);
    lv_label_set_long_mode(Bluetooth_State().name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(Bluetooth_State().name_label, fontpack_lv_font_ui()->line_height);
    Bluetooth_State().version_label = info_row(Lang::Strings::BT_FW_TITLE,
        Lang::Strings::COMMON_UNKNOWN, Lang::Strings::BT_CHECK_VERSION,
        [](lv_event_t*) { QueryVersion(); }, &Bluetooth_State().check_version_btn);
    info_row(Lang::Strings::BT_RESET_TITLE, Lang::Strings::BT_RESET_ACTION,
        nullptr, Bluetooth_on_reset_bt_clicked, nullptr);
    Bluetooth_State().current_mode_label = info_row(Lang::Strings::BT_CURRENT_MODE,
        Lang::Strings::COMMON_UNKNOWN, nullptr, nullptr, nullptr);

    lv_obj_t* mode_title = lv_label_create(parent);
    lv_label_set_text(mode_title, Lang::Strings::BT_MODE_TITLE);
    lv_obj_set_style_text_font(mode_title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(mode_title, lv_color_black(), 0);
    lv_obj_clear_flag(mode_title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* mode_grid = lv_obj_create(parent);
    Bluetooth_strip_container(mode_grid);
    lv_obj_set_width(mode_grid, lv_pct(100));
    lv_obj_set_height(mode_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mode_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mode_grid, kSettingsOptionGap, 0);
    const char* mode_labels[] = {Lang::Strings::BT_MODE1, Lang::Strings::BT_MODE2,
                                Lang::Strings::BT_MODE3, Lang::Strings::BT_MODE5};
    for (int i = 0; i < kBluetoothModeCount; i += 2) {
        auto* row = lv_obj_create(mode_grid);
        Bluetooth_strip_container(row);
        lv_obj_set_size(row, lv_pct(100), kSettingsOptionH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, kSettingsOptionGap, 0);
        for (int j = i; j < i + 2 && j < kBluetoothModeCount; ++j) {
            Bluetooth_State().mode_btns[j] = Bluetooth_make_mode_button(row, mode_labels[j], j);
        }
    }
    auto* description_area = lv_obj_create(parent);
    Bluetooth_strip_container(description_area);
    lv_obj_set_size(description_area, lv_pct(100), UiSy(72));
    auto* description = lv_label_create(description_area);
    Bluetooth_State().mode_description_label = description;
    lv_obj_clear_flag(description, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(description_area, [](lv_event_t* e) {
        auto* area = lv_event_get_target_obj(e);
        auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        ScreenFitLabel(label, lv_obj_get_content_width(area), lv_obj_get_content_height(area));
        lv_obj_center(label);
    }, LV_EVENT_SIZE_CHANGED, description);

    auto* devices_entry = Bluetooth_make_action_button(parent, Lang::Strings::BT_AUDIO_DEVICES,
        [](lv_event_t*) { OpenAudioDevices(); });
    Bluetooth_State().mode2_entry = devices_entry;
    lv_obj_set_width(devices_entry, lv_pct(100));
    lv_obj_add_flag(devices_entry, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_add_event_cb(panel, [](lv_event_t* e) {
        auto* panel = lv_event_get_target_obj(e);
        auto* list = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        const auto available = lv_obj_get_content_height(panel) - 2 * kSettingsOptionH - UiSy(24);
        lv_obj_set_height(list, available > 0 ? available : 1);
    }, LV_EVENT_SIZE_CHANGED, list);

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
    lv_obj_set_size(status, lv_pct(100), UiSy(60));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hid_entry = Bluetooth_make_action_button(parent, Lang::Strings::BT_HID_TITLE,
        [](lv_event_t*) {
            if (Bluetooth_mode_cmd_busy.load(std::memory_order_acquire)) return;
            ScreenNavigateTo(CreateHidTestScreen, SettingsScreen::CreateBluetooth);
        });
    Bluetooth_State().hid_panel = hid_entry;
    lv_obj_set_width(hid_entry, lv_pct(100));

    // 开机 MetalioAudio 已下发模式1；UI 首次进入时对齐选中态
    if (Bluetooth_active_mode == BtMode::kNone) {
        Bluetooth_active_mode = BtMode::kMode1;
    }
    Bluetooth_restore_mode_ui();
}

void BluetoothScreen::ResetUi() {
    CloseAudioDevices();
    if (auto* root = Bluetooth_State().root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_row(root, kSettingsOptionGap, 0);
    }
    CloseNameKeyboard();
    ReleaseHidAlt();
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
    ReleaseHidAlt();
    {
        std::lock_guard<std::mutex> lock(Bluetooth_rx_mutex);
        Bluetooth_rx_buffer.clear();
    }
    auto& uart = SimpleUart::getInstance();
    if (Bluetooth_State().version_label != nullptr) {
        lv_label_set_text(Bluetooth_State().version_label, Lang::Strings::COMMON_UNKNOWN);
    }
    uart.registerCallback(Bluetooth_on_uart_data);
    QueryVersion();
    if (!Bluetooth_query_name()) Bluetooth_post_status(Lang::Strings::BT_NAME_FAILED);
}

void BluetoothScreen::OnDeactivated() {
    ESP_LOGI(TAG, "deactivate: bluetooth tab");
    CloseAudioDevices();
    CloseNameKeyboard();
    ReleaseHidAlt();
    RestoreVersionButton();
    Bluetooth_release_page_audio_power();
    // 设置旧页延迟删除时，HID 测试页已接管同一 UART 回调。
    if (hid_test_screen != nullptr && lv_screen_active() == hid_test_screen) return;
    SimpleUart::getInstance().registerCallback(
        std::function<void(const std::vector<uint8_t>&)>());
    Bluetooth_screen_active = false;
}

bool BluetoothScreen::DismissOverlay() {
    if (name_confirm_panel != nullptr) { CloseNameConfirmation(); return true; }
    if (name_keyboard_panel != nullptr) { CloseNameKeyboard(); return true; }
    if (audio_devices_page != nullptr) { CloseAudioDevices(); return true; }
    return false;
}
