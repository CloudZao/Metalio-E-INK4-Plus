#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <esp_log.h>
#include "assets/lang_bluetooth.h"
#include "assets/lang_config.h"
#include "business/bluetooth/bluetooth_control.h"
#include "ui/apps/settings/bluetooth/bluetooth_screen.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_keyboard.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

static constexpr const char* TAG = "BluetoothScreen";
static constexpr BtMode kBluetoothModes[] = {BtMode::kMode1, BtMode::kMode2, BtMode::kMode3, BtMode::kMode5};
static constexpr int kBluetoothModeCount = sizeof(kBluetoothModes) / sizeof(kBluetoothModes[0]);

struct BluetoothUiState {
    lv_obj_t* root = nullptr;                       // 根容器
    lv_obj_t* status_label = nullptr;               // 状态文案
    lv_obj_t* name_label = nullptr;                 // 经典蓝牙名称
    lv_obj_t* current_mode_label = nullptr;         // 当前模式说明
    lv_obj_t* mode_rows[kBluetoothModeCount] = {};  // 模式选项行
    lv_obj_t* mode_description_label = nullptr;     // 当前选中模式的说明
    lv_obj_t* mode2_entry = nullptr;                // 蓝牙设备管理入口。
    lv_obj_t* mode2_panel = nullptr;                // 模式 2 面板
    lv_obj_t* hid_panel = nullptr;                  // 模式 5 HID 测试入口
    lv_obj_t* device_list = nullptr;                // 设备列表
    lv_obj_t* music_btn = nullptr;                  // 音乐模式按钮
    lv_obj_t* call_btn = nullptr;                   // 通话模式按钮
};

static UiPageLifecycle s_page;
static UiPageLifecycle s_hid_page;
static BluetoothControlSnapshot s_control;
static uint32_t s_devices_revision = 0;
static BtMode s_shown_mode = BtMode::kNone;

static BluetoothUiState& Bluetooth_State();
static void Bluetooth_strip_container(lv_obj_t* object);
static void Bluetooth_update_status_label(const char* text);
static void Bluetooth_show_mode2_panel(bool show);
static void Bluetooth_refresh_mode_buttons();
static void Bluetooth_restore_mode_ui();
static void Bluetooth_clear_device_list_ui();
static void Bluetooth_add_device_to_list(const char* address, const char* name);
static void Bluetooth_reset_ui_state();
static lv_obj_t* Bluetooth_make_action_button(lv_obj_t* parent, const char* label, lv_event_cb_t callback);
static void Bluetooth_on_mode_btn_clicked(lv_event_t* event);
static void Bluetooth_on_call_mode_clicked(lv_event_t* event);
static void Bluetooth_on_music_mode_clicked(lv_event_t* event);
static void Bluetooth_on_reset_bt_clicked(lv_event_t* event);
static void PollBluetoothControl();

static constexpr int kBluetoothContentPad = UiSx(14);  // 蓝牙页面内边距

static BluetoothUiState& Bluetooth_State() {
    static BluetoothUiState s;
    return s;
}

static bool Bluetooth_screen_active = false;

static lv_obj_t* CreateHidTestScreen();
static lv_obj_t* name_keyboard_panel = nullptr;  // 名称键盘遮罩。
static lv_obj_t* name_confirm_panel = nullptr;   // 名称确认遮罩。
static lv_obj_t* name_hint = nullptr;            // 键盘内提示。
static lv_obj_t* name_input = nullptr;           // 静态输入文本。
static lv_obj_t* name_box = nullptr;             // 输入框。
static lv_obj_t* name_keyboard = nullptr;        // 与 Wi-Fi 共用的键盘。
static char edited_name[64] = {};                // 当前编辑内容。
static char pending_name[64] = {};               // 勾选时冻结的待确认名称。

static UiModal s_window_modal;

static void CloseNameConfirmation() {
    if (name_confirm_panel != nullptr) {
        lv_obj_delete(name_confirm_panel);
    }
}

static void CloseNameKeyboard() {
    CloseNameConfirmation();
    if (name_keyboard_panel != nullptr) {
        lv_obj_delete(name_keyboard_panel);
    }
}

static void RefreshNameInput() {
    if (name_input == nullptr) {
        return;
    }
    const char* tail = edited_name;
    const auto* font = UiFont();
    const int width = lv_obj_get_content_width(name_box) - UiSx(12);
    lv_point_t size;
    while (true) {
        lv_text_get_size(&size, tail, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (width <= 0 || size.x <= width || *tail == '\0') {
            break;
        }
        do {
            ++tail;
        } while ((static_cast<unsigned char>(*tail) & 0xC0) == 0x80);
    }
    lv_label_set_text(name_input, tail);
    lv_obj_set_width(name_input, size.x > 0 ? size.x : 1);
}

static void ShowNameConfirmation() {
    if (name_confirm_panel != nullptr) {
        return;
    }
    if (!Bluetooth_is_valid_name(edited_name)) {
        lv_label_set_text(name_hint, LangString_BT_NAME_INVALID);
        return;
    }
    snprintf(pending_name, sizeof(pending_name), "%s", edited_name);
    ScreenKeyboardPause(name_keyboard);
    auto* mask = lv_obj_create(lv_screen_active());
    name_confirm_panel = mask;
    s_window_modal.Bind(mask, [](void*) { CloseNameConfirmation(); });
    Bluetooth_strip_container(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        mask,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
                CloseNameConfirmation();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { name_confirm_panel = nullptr; }, LV_EVENT_DELETE, nullptr);
    auto* card = lv_obj_create(mask);
    Bluetooth_strip_container(card);
    lv_obj_set_width(card, LV_HOR_RES - UiSx(80));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UiSx(20), 0);
    lv_obj_set_style_pad_row(card, UiSy(20), 0);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(card, nullptr, 0);
    lv_obj_set_style_border_width(card, kScreenBorderW, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_radius(card, kScreenOptionRadius, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    auto* message = lv_label_create(card);
    lv_label_set_text_fmt(message, LangString_BT_NAME_CONFIRM_FMT, pending_name);
    lv_obj_set_width(message, lv_pct(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(message, UiFont(), 0);
    lv_obj_set_style_text_color(message, UiColorForeground(), 0);
    lv_obj_clear_flag(message, LV_OBJ_FLAG_CLICKABLE);
    auto* row = lv_obj_create(card);
    Bluetooth_strip_container(row);
    lv_obj_set_size(row, lv_pct(100), kScreenOptionH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, kScreenOptionGap, 0);
    auto* cancel =
        Bluetooth_make_action_button(row, LangString_COMMON_CANCEL, [](lv_event_t*) { CloseNameConfirmation(); });
    auto* confirm = Bluetooth_make_action_button(row, LangString_BT_NAME_CONFIRM, [](lv_event_t*) {
        if (Bluetooth_rename(pending_name)) {
            CloseNameKeyboard();
        } else {
            CloseNameConfirmation();
        }
    });
    for (auto* button : {cancel, confirm}) {
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lv_obj_get_child(button, 0));
    }
}

[[maybe_unused]] static void OpenNameKeyboard() {
    if (name_keyboard_panel != nullptr || BluetoothCommandsBusy()) {
        return;
    }
    auto* mask = lv_obj_create(lv_screen_active());
    name_keyboard_panel = mask;
    Bluetooth_strip_container(mask);
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        mask,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
                CloseNameKeyboard();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        mask,
        [](lv_event_t*) {
            name_keyboard_panel = nullptr;
            name_input = name_hint = name_box = name_keyboard = nullptr;
            edited_name[0] = pending_name[0] = '\0';
        },
        LV_EVENT_DELETE, nullptr);
    auto* panel = lv_obj_create(mask);
    Bluetooth_strip_container(panel);
    lv_obj_set_size(panel, lv_pct(100), ScreenKeyboardPreferredHeight() + UiSy(180) + kBluetoothContentPad * 2);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UiColorForeground(), 0);
    lv_obj_set_style_border_width(panel, kScreenBorderW, 0);
    lv_obj_set_style_pad_all(panel, kBluetoothContentPad, 0);
    lv_obj_set_style_pad_row(panel, UiSy(8), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        panel,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
                CloseNameKeyboard();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    name_hint = lv_label_create(panel);
    lv_label_set_text(name_hint, LangString_BT_NAME_EDIT_HINT);
    lv_obj_set_width(name_hint, lv_pct(100));
    lv_label_set_long_mode(name_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(name_hint, UiFont(), 0);
    lv_obj_set_style_text_color(name_hint, UiColorForeground(), 0);
    lv_obj_clear_flag(name_hint, LV_OBJ_FLAG_CLICKABLE);
    name_box = lv_obj_create(panel);
    Bluetooth_strip_container(name_box);
    lv_obj_set_size(name_box, lv_pct(100), UiSy(64));
    lv_obj_set_style_border_width(name_box, kScreenBorderW, 0);
    lv_obj_set_style_border_color(name_box, UiColorForeground(), 0);
    lv_obj_set_style_pad_hor(name_box, UiSx(10), 0);
    lv_obj_set_flex_flow(name_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(name_box, LV_OBJ_FLAG_CLICKABLE);
    name_input = lv_label_create(name_box);
    lv_label_set_long_mode(name_input, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name_input, UiFont(), 0);
    lv_obj_set_style_text_color(name_input, UiColorForeground(), 0);
    lv_obj_clear_flag(name_input, LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_precise_t cursor_points[] = {
        {0, 0}, {UiSx(8), 0}, {UiSx(4), 0}, {UiSx(4), UiSy(38)}, {0, UiSy(38)}, {UiSx(8), UiSy(38)}};
    auto* cursor = lv_line_create(name_box);
    lv_line_set_points(cursor, cursor_points, 6);
    lv_obj_set_style_line_width(cursor, UiSx(2), 0);
    lv_obj_set_style_line_color(cursor, UiColorForeground(), 0);
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
    auto* label = Bluetooth_State().name_label;
    edited_name[0] = '\0';
    if (label != nullptr && strcmp(lv_label_get_text(label), LangString_COMMON_UNKNOWN) != 0) {
        snprintf(edited_name, sizeof(edited_name), "%s", lv_label_get_text(label));
    }
    name_keyboard = ScreenKeyboardCreate(panel, [](ScreenKeyboardEvent event, const char* text, void*) {
        size_t length = strlen(edited_name);
        switch (event) {
            case ScreenKeyboardEvent::kInsert:
                if (length + strlen(text) < sizeof(edited_name)) {
                    strcat(edited_name, text);
                }
                break;
            case ScreenKeyboardEvent::kBackspace:
                if (length > 0) {
                    do {
                        --length;
                    } while (length > 0 && (static_cast<unsigned char>(edited_name[length]) & 0xC0) == 0x80);
                    edited_name[length] = '\0';
                }
                break;
            case ScreenKeyboardEvent::kClear:
                edited_name[0] = '\0';
                break;
            case ScreenKeyboardEvent::kConfirm:
                ShowNameConfirmation();
                return;
            case ScreenKeyboardEvent::kLayout:
                break;
        }
        RefreshNameInput();
    });
    if (name_keyboard == nullptr) {
        CloseNameKeyboard();
        return;
    }
    lv_obj_set_height(name_keyboard, 0);
    lv_obj_set_flex_grow(name_keyboard, 1);
    ScreenKeyboardReset(name_keyboard);
    lv_obj_update_layout(panel);
    RefreshNameInput();
}

static lv_obj_t* hid_test_screen = nullptr;       // HID 全屏测试页。
static lv_obj_t* hid_test_status = nullptr;       // HID 测试页独立状态栏。
static bool hid_alt_held = false;                 // UI 线程记录尚未释放的 Alt。
static lv_obj_t* audio_devices_page = nullptr;    // 全屏音频设备管理页。
static lv_obj_t* audio_devices_status = nullptr;  // 管理页状态文案。

static void CloseAudioDevices() {
    if (audio_devices_page == nullptr) {
        return;
    }
    auto* panel = Bluetooth_State().mode2_panel;
    if (panel != nullptr && Bluetooth_State().root != nullptr) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(panel, Bluetooth_State().root);
        lv_obj_set_flex_grow(panel, 0);
        lv_obj_set_height(panel, UiSy(230));
    }
    lv_obj_delete(audio_devices_page);
}

static void OpenAudioDevices() {
    auto* panel = Bluetooth_State().mode2_panel;
    if (audio_devices_page != nullptr || panel == nullptr || BluetoothCurrentMode() != BtMode::kMode2 ||
        BluetoothCommandsBusy()) {
        return;
    }
    BluetoothScanDevices();
    auto* page = lv_obj_create(lv_screen_active());
    audio_devices_page = page;
    Bluetooth_strip_container(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES - ScreenStatusBarHeight());
    lv_obj_align(page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(page, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, kBluetoothContentPad, 0);
    lv_obj_set_style_pad_row(page, kScreenOptionGap, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(
        page, [](lv_event_t*) { audio_devices_page = audio_devices_status = nullptr; }, LV_EVENT_DELETE, nullptr);
    auto* header = lv_obj_create(page);
    Bluetooth_strip_container(header);
    lv_obj_set_size(header, lv_pct(100), UiSy(52));
    auto* back = SettingsCreateBackButton(header, [](lv_event_t*) { CloseAudioDevices(); });
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    auto* title = lv_label_create(header);
    lv_label_set_text(title, LangString_BT_AUDIO_DEVICES);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(title);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_parent(panel, page);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(panel, 0);
    lv_obj_set_flex_grow(panel, 1);
    audio_devices_status = lv_label_create(page);
    lv_obj_set_size(audio_devices_status, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(audio_devices_status, UiSy(80), 0);
    lv_label_set_long_mode(audio_devices_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(audio_devices_status, LangString_BT_MODE2_SET);
    lv_obj_set_style_text_font(audio_devices_status, UiFont(), 0);
    lv_obj_set_style_text_color(audio_devices_status, UiColorForeground(), 0);
    lv_obj_clear_flag(audio_devices_status, LV_OBJ_FLAG_CLICKABLE);
}

static bool ReleaseHidAlt() {
    if (!hid_alt_held) {
        return true;
    }
    if (!Bluetooth_send_hid_shortcut("ALT,UP")) {
        return false;
    }
    hid_alt_held = false;
    return true;
}

static bool OnHidTestNavigationKey(const char* key) {
    if (key == nullptr || strcmp(key, "vk_home") != 0) {
        return false;
    }
    if (ReleaseHidAlt()) {
        ScreenNavigateBack();
    }
    return true;
}

static void OnHidAltEvent(lv_event_t* e) {
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (!hid_alt_held && Bluetooth_screen_active) {
            hid_alt_held = Bluetooth_send_hid_shortcut("ALT,DOWN");
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
        ReleaseHidAlt();
    }
}

static void Bluetooth_strip_container(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void Bluetooth_update_status_label(const char* text) {
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

static void Bluetooth_refresh_mode_buttons() {
    if (auto* label = Bluetooth_State().current_mode_label) {
        const char* description = LangString_COMMON_UNKNOWN;
        switch (BluetoothCurrentMode()) {
            case BtMode::kMode1:
                description = LangString_BT_MODE1;
                break;
            case BtMode::kMode2:
                description = LangString_BT_MODE2;
                break;
            case BtMode::kMode3:
                description = LangString_BT_MODE3;
                break;
            case BtMode::kMode5:
                description = LangString_BT_MODE5;
                break;
            default:
                break;
        }
        lv_label_set_text(label, description);
    }
    if (auto* label = Bluetooth_State().mode_description_label) {
        const char* description = LangString_COMMON_UNKNOWN;
        switch (BluetoothCurrentMode()) {
            case BtMode::kMode1:
                description = LangString_BT_MODE1_DESC;
                break;
            case BtMode::kMode2:
                description = LangString_BT_MODE2_DESC;
                break;
            case BtMode::kMode3:
                description = LangString_BT_MODE3_DESC;
                break;
            case BtMode::kMode5:
                description = LangString_BT_MODE5_DESC;
                break;
            default:
                break;
        }
        lv_label_set_text(label, description);
    }
    if (BluetoothCurrentMode() != BtMode::kMode5) {
        ReleaseHidAlt();
    }
    auto* hid_panel = Bluetooth_State().hid_panel;
    if (hid_panel != nullptr) {
        if (BluetoothCurrentMode() == BtMode::kMode5) {
            lv_obj_clear_flag(hid_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hid_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < kBluetoothModeCount; ++i) {
        if (Bluetooth_State().mode_rows[i] == nullptr) {
            continue;
        }
        const bool active = (BluetoothCurrentMode() == kBluetoothModes[i]);
        SettingsSetListRowCurrent(Bluetooth_State().mode_rows[i], active);
    }
}

static void Bluetooth_show_mode2_panel(bool show) {
    auto* entry = Bluetooth_State().mode2_entry;
    if (entry != nullptr) {
        if (show) {
            lv_obj_clear_flag(entry, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(entry, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!show) {
        CloseAudioDevices();
    }
}

static void Bluetooth_clear_device_list_ui() {
    if (Bluetooth_State().device_list == nullptr) {
        return;
    }
    lv_obj_clean(Bluetooth_State().device_list);
}

static void Bluetooth_add_device_to_list(const char* address, const char* name) {
    if (Bluetooth_State().device_list == nullptr) {
        return;
    }

    lv_obj_t* item = lv_obj_create(Bluetooth_State().device_list);
    lv_obj_remove_style_all(item);
    lv_obj_set_width(item, lv_pct(100));
    lv_obj_set_height(item, kScreenOptionH);
    lv_obj_set_style_pad_hor(item, kScreenOptionPad, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(item);

    char* addr_copy = static_cast<char*>(lv_malloc(kAddrHexLen + 1));
    if (addr_copy != nullptr) {
        memcpy(addr_copy, address, kAddrHexLen + 1);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                const char* addr = static_cast<const char*>(lv_event_get_user_data(e));
                if (addr == nullptr) {
                    return;
                }
                BluetoothConnectDevice(addr);
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
    if (name[0] != '\0') {
        snprintf(display, sizeof(display), "%s\n%s", name, address);
    } else {
        snprintf(display, sizeof(display), "%s", address);
    }
    lv_label_set_text(lbl, display);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    ScreenStyleSelectable(item, lbl, false);

}

static void Bluetooth_on_mode_btn_clicked(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= kBluetoothModeCount) {
        return;
    }
    if (!ReleaseHidAlt()) {
        return;
    }
    Bluetooth_send_mode_command(kBluetoothModes[idx]);
}

static void Bluetooth_on_reset_bt_clicked(lv_event_t*) {
    if (!ReleaseHidAlt() || BluetoothCommandsBusy()) {
        return;
    }
    Bluetooth_update_status_label(LangString_BT_PWR_RESETTING);
    const auto sequence = EpdGetStats().submitted + 1;
    if (!BluetoothResetModule([sequence] { return EpdWait(sequence, 5000); })) {
        Bluetooth_update_status_label(LangString_BT_PWR_RESET_FAILED);
    }
}

static void Bluetooth_on_call_mode_clicked(lv_event_t*) {
    BluetoothSetCallMode(true);
}

static void Bluetooth_on_music_mode_clicked(lv_event_t*) {
    BluetoothSetCallMode(false);
}

static lv_obj_t* CreateHidTestScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    hid_test_screen = screen;
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    lv_obj_set_style_text_color(screen, UiColorForeground(), 0);
    const auto status_bar = ScreenCreateStatusBar(screen);

    lv_obj_t* page = lv_obj_create(screen);
    Bluetooth_strip_container(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES - status_bar.height);
    lv_obj_align(page, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(page, kBluetoothContentPad, 0);
    lv_obj_set_style_pad_row(page, kScreenOptionGap, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    for (const char* text : {LangString_BT_HID_TITLE, LangString_BT_HID_HINT}) {
        lv_obj_t* label = lv_label_create(page);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        if (text == LangString_BT_HID_TITLE) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    lv_obj_t* keys = lv_obj_create(page);
    Bluetooth_strip_container(keys);
    lv_obj_set_width(keys, lv_pct(100));
    lv_obj_set_height(keys, 0);
    lv_obj_set_flex_grow(keys, 1);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(keys, kScreenOptionGap, 0);
    lv_obj_add_flag(keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(keys, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(keys, LV_SCROLLBAR_MODE_AUTO);
    const char* labels[] = {"A",
                            "B",
                            "1",
                            LangString_BT_HID_ENTER,
                            LangString_BT_HID_SPACE,
                            LangString_BT_HID_TAB,
                            LangString_BT_HID_ESC,
                            LangString_BT_HID_BACKSPACE,
                            LangString_BT_HID_DELETE,
                            LangString_BT_HID_LEFT,
                            LangString_BT_HID_RIGHT,
                            LangString_BT_HID_UP,
                            LangString_BT_HID_DOWN,
                            "Ctrl+A",
                            "Ctrl+C",
                            "Ctrl+V",
                            "Alt+Tab",
                            "Win+D"};
    static const char* const shortcuts[] = {"A",    "B",         "1",      "ENTER",  "SPACE",   "TAB",
                                            "ESC",  "BACKSPACE", "DELETE", "LEFT",   "RIGHT",   "UP",
                                            "DOWN", "CTRL+A",    "CTRL+C", "CTRL+V", "ALT+TAB", "WIN+D"};
    for (int i = 0; i < 18; i += 3) {
        lv_obj_t* row = lv_obj_create(keys);
        Bluetooth_strip_container(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kScreenOptionH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, kScreenOptionGap, 0);
        for (int j = i; j < i + 3; ++j) {
            lv_obj_t* button = Bluetooth_make_action_button(row, labels[j], [](lv_event_t* e) {
                const char* shortcut = static_cast<const char*>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
                Bluetooth_send_hid_shortcut(shortcut);
            });
            lv_obj_set_user_data(button, const_cast<char*>(shortcuts[j]));
            lv_obj_set_width(button, 0);
            lv_obj_set_flex_grow(button, 1);
            lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lv_obj_get_child(button, 0));
        }
    }
    lv_obj_t* alt_button = Bluetooth_make_action_button(page, LangString_BT_HID_HOLD_ALT, OnHidAltEvent);
    lv_obj_set_width(alt_button, lv_pct(100));
    for (auto code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_DELETE}) {
        lv_obj_add_event_cb(alt_button, OnHidAltEvent, code, nullptr);
    }
    lv_obj_t* release = Bluetooth_make_action_button(page, LangString_BT_HID_RELEASE, [](lv_event_t*) {
        if (Bluetooth_send_hid_shortcut("RELEASE")) {
            hid_alt_held = false;
        }
    });
    lv_obj_set_width(release, lv_pct(100));
    hid_test_status = lv_label_create(page);
    lv_obj_set_width(hid_test_status, lv_pct(100));
    lv_obj_set_height(hid_test_status, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hid_test_status, UiSy(88), 0);
    lv_label_set_long_mode(hid_test_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hid_test_status, LangString_BT_HID_READY);
    lv_obj_clear_flag(hid_test_status, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(
        screen,
        [](lv_event_t* e) {
            const auto code = lv_event_get_code(e);
            if (code == LV_EVENT_SCREEN_LOADED) {
                Bluetooth_screen_active = true;
                BluetoothStartControl();
                s_hid_page.Bind(hid_test_screen, {PollBluetoothControl, [](UiPagePowerEvent event) {
                    if (event == UiPagePowerEvent::PauseViews) {
                        ReleaseHidAlt();
                    }
                }, [] { ReleaseHidAlt(); }, 150});
            } else if (code == LV_EVENT_SCREEN_UNLOADED) {
                ReleaseHidAlt();
                hid_test_status = nullptr;
                auto* root = Bluetooth_State().root;
                // 返回设置页时，新页已接管 UART，不能由旧测试页注销。
                if (root == nullptr || lv_obj_get_screen(root) != lv_screen_active()) {
                    Bluetooth_screen_active = false;
                    Bluetooth_release_page_audio_power();
                }
            } else if (code == LV_EVENT_DELETE) {
                if (lv_event_get_target_obj(e) == hid_test_screen) {
                    hid_test_screen = nullptr;
                }
            }
        },
        LV_EVENT_ALL, nullptr);
    ScreenSetIsHome(false);
    VkKeyScreenDesc navigation{CreateHidTestScreen};
    navigation.on_key = OnHidTestNavigationKey;
    navigation.on_key_long_press = OnHidTestNavigationKey;
    VkKey_AttachScreen(screen, "bluetooth_hid_test", navigation);
    return screen;
}

void BluetoothScreen::BuildInto(lv_obj_t* parent) {
    s_control.revision = 0;
    s_devices_revision = 0;
    s_shown_mode = BtMode::kNone;
    s_page.Bind(parent, {PollBluetoothControl, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseViews) {
            ReleaseHidAlt();
        }
    }, BluetoothScreen::OnDeactivated, 150}, true);
    Bluetooth_State().root = parent;
    lv_obj_set_style_pad_row(parent, kSettingsSectionGap, 0);
    lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);

    auto* info = SettingsCreateListGroup(parent);
    auto info_row = [info](const char* title, const char* value, lv_event_cb_t callback) {
        auto* row = SettingsCreateListRow(info, title, value, callback);
        auto* value_label = lv_obj_get_child(row, 1);
        return value_label;
    };
    Bluetooth_State().name_label =
        info_row(LangString_BT_NAME_TITLE, LangString_COMMON_UNKNOWN, nullptr);
    lv_label_set_long_mode(Bluetooth_State().name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(Bluetooth_State().name_label, UiFont(UiFontRole::Caption)->line_height);
    info_row(LangString_BT_RESET_TITLE, LangString_BT_RESET_ACTION, Bluetooth_on_reset_bt_clicked);
    Bluetooth_State().current_mode_label =
        info_row(LangString_BT_CURRENT_MODE, LangString_COMMON_UNKNOWN, nullptr);

    auto* mode_section = SettingsCreateSection(parent, LangString_BT_MODE_TITLE);

    auto* mode_group = SettingsCreateListGroup(mode_section);
    const char* mode_labels[] = {LangString_BT_MODE1, LangString_BT_MODE2, LangString_BT_MODE3, LangString_BT_MODE5};
    for (int i = 0; i < kBluetoothModeCount; ++i) {
        Bluetooth_State().mode_rows[i] =
            SettingsCreateListRow(mode_group, mode_labels[i], "", Bluetooth_on_mode_btn_clicked, i, false);
    }
    SettingsCreateSectionHint(mode_section, LangString_COMMON_UNKNOWN);
    Bluetooth_State().mode_description_label = lv_obj_get_child(mode_section, -1);
    lv_obj_set_style_text_align(Bluetooth_State().mode_description_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_align(Bluetooth_State().mode_description_label, LV_ALIGN_TOP_LEFT, 0);

    auto* devices_entry =
        Bluetooth_make_action_button(parent, LangString_BT_AUDIO_DEVICES, [](lv_event_t*) { OpenAudioDevices(); });
    Bluetooth_State().mode2_entry = devices_entry;
    lv_obj_set_width(devices_entry, lv_pct(100));
    lv_obj_add_flag(devices_entry, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* panel = lv_obj_create(parent);
    Bluetooth_State().mode2_panel = panel;
    Bluetooth_strip_container(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, UiSy(230));
    lv_obj_set_style_border_width(panel, kScreenBorderW, 0);
    lv_obj_set_style_border_color(panel, UiColorForeground(), 0);
    lv_obj_set_style_radius(panel, kScreenOptionRadius, 0);
    lv_obj_set_style_pad_all(panel, UiSx(8), 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* list = lv_obj_create(panel);
    Bluetooth_State().device_list = list;
    Bluetooth_strip_container(list);
    lv_obj_set_size(list, lv_pct(100), UiSy(140));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_border_width(list, kScreenBorderW, 0);
    lv_obj_set_style_border_color(list, UiColorForeground(), 0);
    lv_obj_set_style_radius(list, kScreenOptionRadius, 0);
    lv_obj_set_style_pad_all(list, UiSx(6), 0);
    lv_obj_set_style_pad_row(list, UiSy(6), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_event_cb(
        panel,
        [](lv_event_t* e) {
            auto* panel = lv_event_get_target_obj(e);
            auto* list = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            const auto available = lv_obj_get_content_height(panel) - kScreenOptionH - UiSy(16);
            lv_obj_set_height(list, available > 0 ? available : 1);
        },
        LV_EVENT_SIZE_CHANGED, list);

    lv_obj_t* btn_row = lv_obj_create(panel);
    Bluetooth_strip_container(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, kScreenOptionH);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, kScreenOptionGap, 0);

    lv_obj_t* music = Bluetooth_make_action_button(btn_row, LangString_BT_MUSIC_BTN, Bluetooth_on_music_mode_clicked);
    Bluetooth_State().music_btn = music;
    lv_obj_set_flex_grow(music, 1);

    lv_obj_t* call = Bluetooth_make_action_button(btn_row, LangString_BT_CALL_BTN, Bluetooth_on_call_mode_clicked);
    Bluetooth_State().call_btn = call;
    lv_obj_set_flex_grow(call, 1);

    lv_obj_t* status = lv_label_create(parent);
    Bluetooth_State().status_label = status;
    lv_label_set_text(status, LangString_BT_SELECT_MODE);
    lv_obj_set_style_text_font(status, UiFont(), 0);
    lv_obj_set_style_text_color(status, UiColorForeground(), 0);
    lv_obj_set_size(status, lv_pct(100), UiSy(60));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hid_entry = Bluetooth_make_action_button(parent, LangString_BT_HID_TITLE, [](lv_event_t*) {
        if (BluetoothCommandsBusy()) {
            return;
        }
        ScreenNavigateTo(CreateHidTestScreen, SettingsScreen::CreateBluetooth);
    });
    Bluetooth_State().hid_panel = hid_entry;
    lv_obj_set_width(hid_entry, lv_pct(100));

    Bluetooth_restore_mode_ui();
}

void BluetoothScreen::ResetUi() {
    s_page.Unbind();
    CloseAudioDevices();
    CloseNameKeyboard();
    ReleaseHidAlt();
    Bluetooth_reset_ui_state();
}

void BluetoothScreen::OnActivated() {
    ESP_LOGI(TAG, "activate: bluetooth tab");
    Bluetooth_screen_active = true;
    ReleaseHidAlt();
    BluetoothStartControl();
    if (!Bluetooth_query_name()) {
        Bluetooth_update_status_label(LangString_BT_NAME_FAILED);
    }
}

void BluetoothScreen::OnDeactivated() {
    s_page.Unbind();
    ESP_LOGI(TAG, "deactivate: bluetooth tab");
    CloseAudioDevices();
    CloseNameKeyboard();
    ReleaseHidAlt();
    // 设置旧页延迟删除时，HID 测试页已接管同一 UART 回调。
    if (hid_test_screen != nullptr && lv_screen_active() == hid_test_screen) {
        return;
    }
    Bluetooth_release_page_audio_power();
    Bluetooth_screen_active = false;
}

bool BluetoothScreen::DismissOverlay() {

    if (name_keyboard_panel != nullptr) {
        CloseNameKeyboard();
        return true;
    }
    if (audio_devices_page != nullptr) {
        CloseAudioDevices();
        return true;
    }
    return false;
}

static void Bluetooth_reset_ui_state() {
    Bluetooth_State() = {};
    s_control = {};
}

static lv_obj_t* Bluetooth_make_action_button(lv_obj_t* parent, const char* label, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, kScreenOptionH);
    lv_obj_set_style_pad_hor(btn, kScreenOptionPad, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    ScreenStyleSelectable(btn, lbl, false);
    return btn;
}

static void Bluetooth_restore_mode_ui() {
    Bluetooth_refresh_mode_buttons();
    switch (BluetoothCurrentMode()) {
        case BtMode::kMode1:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(LangString_BT_MODE1_SET);
            break;
        case BtMode::kMode2:
            Bluetooth_show_mode2_panel(true);
            Bluetooth_update_status_label(LangString_BT_MODE2_SET);
            break;
        case BtMode::kMode3:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(LangString_BT_MODE3_SET);
            break;
        case BtMode::kMode5:
            Bluetooth_show_mode2_panel(false);
            Bluetooth_update_status_label(LangString_BT_MODE5_SET);
            break;
        default:
            Bluetooth_show_mode2_panel(false);
            break;
    }
}

static void PollBluetoothControl() {
    if (!BluetoothReadControl(s_control)) {
        return;
    }
    const auto mode = BluetoothCurrentMode();
    if (s_shown_mode != mode) {
        s_shown_mode = mode;
        Bluetooth_restore_mode_ui();
    }
    if (s_control.name[0] && Bluetooth_State().name_label) {
        lv_label_set_text(Bluetooth_State().name_label, s_control.name);
    }
    if (Bluetooth_State().device_list && s_devices_revision != s_control.devices_revision) {
        s_devices_revision = s_control.devices_revision;
        Bluetooth_clear_device_list_ui();
        for (const auto& device : s_control.devices) {
            Bluetooth_add_device_to_list(device.address, device.name);
        }
    }
    if (s_control.status[0]) {
        Bluetooth_update_status_label(s_control.status);
    }
}
