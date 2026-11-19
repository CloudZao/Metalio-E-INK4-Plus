/**
 * @file settings_test_screens.cc
 * @brief S31 测试子页：自动 / 老化（触摸、电池见独立文件；4G 双卡测试）
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <time.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include "assets/lang_bluetooth.h"
#include "assets/lang_cellular.h"
#include "assets/lang_screen.h"
#include "application.h"
#include "panel_vcom.h"
#include "settings_test_screens.h"
#include "settings_test_margin_screen.h"
#include "settings_test_audio.h"
#include "settings_test_camera.h"
#include "settings_test_common.h"
#include "settings_test_cell.h"
#include "settings_test_wifi.h"
#include "assets/lang_config.h"
#include "board.h"
#include "power_policy.h"
#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "metalio_sd.h"
#include "metalio_audio.h"
#include "pcf8563.h"
#include "sc7a20h.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "settings_common.h"
#include "settings_network_signal.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

#define TAG "SettingsTest"

static constexpr lv_coord_t kRowH = UiSy(40);

static lv_obj_t* MakeScr(const char* title) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, UiSy(12), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    auto* header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), kRowH);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    auto* list = lv_obj_create(body);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_user_data(scr, body);
    ScreenSetIsHome(false);
    return scr;
}

static lv_obj_t* BodyOf(lv_obj_t* scr) {
    return static_cast<lv_obj_t*>(lv_obj_get_user_data(scr));
}

static void UpdateAutoTestRowHeight(lv_obj_t* row) {
    int content_height = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        auto* child = lv_obj_get_child(row, i);
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            content_height = std::max(content_height, static_cast<int>(lv_obj_get_height(child)));
        }
    }
    const auto sides = lv_obj_get_style_border_side(row, LV_PART_MAIN);
    const int border = lv_obj_get_style_border_width(row, LV_PART_MAIN);
    const int padding = lv_obj_get_style_pad_top(row, LV_PART_MAIN) +
                        lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
    const int border_height = ((sides & LV_BORDER_SIDE_TOP) ? border : 0) +
                              ((sides & LV_BORDER_SIDE_BOTTOM) ? border : 0);
    const int height = std::max<int>(UiSy(44), content_height + padding + border_height);
    // 显式行高使 flex 的交叉方向居中生效，长文本仍按实际高度扩展。
    if (lv_obj_get_style_height(row, LV_PART_MAIN) != height) {
        lv_obj_set_height(row, height);
    }
}

static void LayoutAutoTestRows(lv_obj_t* list) {
    lv_obj_update_layout(list);
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        auto* row = lv_obj_get_child(list, i);
        const auto sides = i == 0 ? LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_BOTTOM;
        lv_obj_set_style_border_side(row, static_cast<lv_border_side_t>(sides), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_ver(row, UiSy(2), 0);
        for (uint32_t j = 0; j < lv_obj_get_child_count(row); ++j) {
            lv_obj_add_event_cb(lv_obj_get_child(row, j), [](lv_event_t* e) {
                UpdateAutoTestRowHeight(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
            }, LV_EVENT_SIZE_CHANGED, row);
        }
        UpdateAutoTestRowHeight(row);
    }
    lv_obj_update_layout(list);
}

static lv_obj_t* AddStatusRow(lv_obj_t* parent, const char* name, const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, kRowH, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* n = lv_label_create(row);
    lv_label_set_text(n, name);
    lv_obj_set_width(n, UiSx(112));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(n, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(n, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_width(v, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(v, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static constexpr const char* kAutoScreenId = "settings_test_auto";
static lv_obj_t* s_auto_scr = nullptr;
static int s_light_step = 0; // 下次点击依次选择冷暖、冷、暖、关闭
static bool s_light_changed = false; // 测试结束时恢复进入测试前的前光状态
static FrontlightCct s_saved_cct = FrontlightCct::kOff;
static uint8_t s_saved_brightness = 0;
static uint8_t s_saved_warmth = 50;

static lv_timer_t* s_auto_info_timer = nullptr;
static lv_obj_t* s_vcom_value = nullptr;
static lv_obj_t* s_vcom_query = nullptr;
static lv_obj_t* s_bt_version = nullptr;
static lv_obj_t* s_sd_capacity = nullptr;
static bool s_bt_query_sent = false;
static bool s_bt_query_done = false;
static int64_t s_bt_query_deadline = 0;

struct SettingsTestCapacityResult {
    uint32_t generation; // 所属自动测试会话
    char text[128]; // SD 状态及容量
};

static void ApplySdCapacity(void* arg) {
    auto* result = static_cast<SettingsTestCapacityResult*>(arg);
    if (SettingsTest_IsLive() && result->generation == SettingsTest_Generation() && s_sd_capacity != nullptr) {
        lv_label_set_text(s_sd_capacity, result->text);
    }
    delete result;
}

static void QuerySdCapacity() {
    auto* result = new (std::nothrow) SettingsTestCapacityResult{SettingsTest_Generation(), {}};
    if (result == nullptr) {
        ESP_LOGE(TAG, "SD capacity result allocation failed");
        lv_label_set_text(s_sd_capacity, Lang::Strings::SETTINGS_STORAGE_CAP_FAIL);
        return;
    }
    if (!Application::GetInstance().Schedule([result]() {
            if (!SettingsTest_IsLive() || result->generation != SettingsTest_Generation()) {
                delete result;
                return;
            }
            uint64_t total = 0;
            uint64_t available = 0;
            const esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &available);
            if (err == ESP_OK) {
                constexpr double gb = 1024.0 * 1024.0 * 1024.0;
                char capacity[96];
                std::snprintf(capacity, sizeof(capacity), Lang::Strings::SETTINGS_TEST_SD_CAP_FMT,
                              static_cast<double>(available) / gb, static_cast<double>(total) / gb);
                std::snprintf(result->text, sizeof(result->text), "%s\n%s", Lang::Strings::SETTINGS_TEST_OK, capacity);
            } else {
                ESP_LOGW(TAG, "SD capacity query failed err=%d", static_cast<int>(err));
                std::snprintf(result->text, sizeof(result->text), "%s", Lang::Strings::SETTINGS_STORAGE_CAP_FAIL);
            }
            if (!ScreenLvAsync(ApplySdCapacity, result)) {
                ESP_LOGW(TAG, "SD capacity UI enqueue failed");
                delete result;
            }
        }, Application::TaskDomain::Storage)) {
        ESP_LOGE(TAG, "SD capacity task scheduling failed");
        delete result;
        lv_label_set_text(s_sd_capacity, Lang::Strings::SETTINGS_STORAGE_CAP_FAIL);
    }
}

static void RefreshBluetoothVersion() {
    if (s_bt_query_done) {
        return;
    }
    char version[64];
    if (s_bt_query_sent && MetalioAudio_GetVersion(version, sizeof(version))) {
        lv_label_set_text(s_bt_version, version);
        s_bt_query_done = true;
        return;
    }
    if (esp_timer_get_time() >= s_bt_query_deadline) {
        ESP_LOGW(TAG, "Bluetooth version query timed out sent=%d", s_bt_query_sent);
        lv_label_set_text(s_bt_version, Lang::Strings::COMMON_UNKNOWN);
        s_bt_query_done = true;
        return;
    }
    if (!s_bt_query_sent && MetalioAudio_ModeReady()) {
        const esp_err_t err = MetalioAudio_QueryVersion();
        if (err == ESP_OK) {
            s_bt_query_sent = true;
            s_bt_query_deadline = esp_timer_get_time() + 5000000;
        } else if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Bluetooth version query failed err=%d", static_cast<int>(err));
            lv_label_set_text(s_bt_version, Lang::Strings::COMMON_UNKNOWN);
            s_bt_query_done = true;
        }
    }
}

static lv_obj_t* AddTestButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback) {
    auto* button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, UiSx(112), UiSy(40));
    lv_obj_set_style_border_width(button, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_radius(button, kSettingsOptionRadius, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
}

static void RefreshAutoInfo(lv_timer_t*) {
    const auto state = PanelVcomGetState();
    char value[32];
    if (state.vcom_mv) {
        std::snprintf(value, sizeof(value), "%d mV", state.vcom_mv);
    } else {
        std::snprintf(value, sizeof(value), "%s", Lang::Strings::SETTINGS_VCOM_UNKNOWN);
    }
    if (std::strcmp(lv_label_get_text(s_vcom_value), value)) lv_label_set_text(s_vcom_value, value);
    if (state.busy) lv_obj_add_state(s_vcom_query, LV_STATE_DISABLED);
    else lv_obj_clear_state(s_vcom_query, LV_STATE_DISABLED);
    RefreshBluetoothVersion();
}

static void OnLightTestClicked(lv_event_t* e) {
    auto& light = Frontlight::GetInstance();
    if (!light.initialized()) return;
    if (!s_light_changed) {
        s_saved_cct = light.cct();
        s_saved_brightness = light.target_brightness();
        s_saved_warmth = light.warmth();
        s_light_changed = true;
    }
    constexpr FrontlightCct modes[] = {
        FrontlightCct::kBoth, FrontlightCct::kCool, FrontlightCct::kWarm, FrontlightCct::kOff,
    };
    const char* names[] = {
        Lang::Strings::SETTINGS_FRONTLIGHT_BOTH, Lang::Strings::SETTINGS_FRONTLIGHT_COOL,
        Lang::Strings::SETTINGS_FRONTLIGHT_WARM, Lang::Strings::SETTINGS_FRONTLIGHT_OFF,
    };
    if (s_light_step != 3 && light.target_brightness() == 0) light.SetBrightness(40);
    light.SetCct(modes[s_light_step]);
    auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_label_set_text(label, names[s_light_step]);
    s_light_step = (s_light_step + 1) % 4;
}

static void OnAutoDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_auto_scr) {
        return;
    }
    if (s_auto_info_timer) lv_timer_delete(s_auto_info_timer);
    s_auto_info_timer = nullptr;
    s_vcom_value = nullptr;
    s_vcom_query = nullptr;
    s_bt_version = nullptr;
    s_sd_capacity = nullptr;
    PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    SettingsTest_BeginShutdown();
    SettingsTestCamera_Teardown();
    SettingsTestWifi_Teardown();
    SettingsTestCell_StopStartTimer();
    SettingsTestCell_StopRetryTimer();
    SettingsTestAudio_Teardown();
    if (s_light_changed) {
        auto& light = Frontlight::GetInstance();
        light.SetBrightness(s_saved_brightness);
        light.SetWarmth(s_saved_warmth);
        if (s_saved_cct == FrontlightCct::kOff) light.SetCct(FrontlightCct::kOff);
        s_light_changed = false;
    }
    s_auto_scr = nullptr;
}

lv_obj_t* SettingsTestAutoScreen::Create() {
    lv_obj_t* scr = MakeScr(Lang::Strings::SETTINGS_TEST_AUTO);
    s_auto_scr = scr;
    s_light_step = 0;
    s_light_changed = false;
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    lv_obj_t* container = BodyOf(scr);
    lv_obj_t* body = lv_obj_get_child(container, 1);
    auto* adjust = SettingsCreateSelectableOption(lv_obj_get_child(container, 0), Lang::Strings::SETTINGS_TEST_MARGIN,
        [](lv_event_t*) { ScreenNavigateTo(SettingsTestMarginScreen::Create); }, 0);
    lv_obj_set_size(adjust, UiSx(152), UiSy(40));
    lv_obj_add_event_cb(scr, OnAutoDeleted, LV_EVENT_DELETE, nullptr);

    auto set_row = [&](const char* name, bool ok, const char* detail) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      ok ? Lang::Strings::SETTINGS_TEST_OK : Lang::Strings::SETTINGS_TEST_FAIL,
                      detail != nullptr ? detail : "");
        return AddStatusRow(body, name, buf);
    };

    {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        const bool ok = Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
        char d[32];
        if (ok) {
            std::snprintf(d, sizeof(d), "%d%%", level);
        } else {
            std::snprintf(d, sizeof(d), "--");
        }
        set_row("BQ27220", ok, d);
    }

    {
        struct tm dt = {};
        const bool ok = Pcf8563::GetInstance().GetTime(dt);
        char d[40] = {};
        if (ok) {
            std::snprintf(d, sizeof(d), "%04d-%02d-%02d %02d:%02d", dt.tm_year + 1900, dt.tm_mon + 1,
                          dt.tm_mday, dt.tm_hour, dt.tm_min);
        }
        set_row("PCF8563", ok, d);
    }

    {
        const bool ok = cx25601n_is_ready();
        set_row("CX25601N", ok, ok ? "" : Lang::Strings::SETTINGS_TEST_NOT_DETECTED);
    }

    {
        const bool ok = MetalioSd_Ok();
        s_sd_capacity = set_row(Lang::Strings::SETTINGS_TEST_SDCARD, ok,
                               ok ? "" : Lang::Strings::SETTINGS_TEST_SD_MOUNT_FAIL);
    }

    {
        int ax = 0, ay = 0, az = 0;
        const bool ok = Sc7a20h::GetInstance().ReadAccelMg(ax, ay, az);
        char d[48] = {};
        if (ok) {
            std::snprintf(d, sizeof(d), "%d,%d,%d mg", ax, ay, az);
        }
        set_row("SC7A20H", ok, d);
    }

    s_vcom_value = AddStatusRow(body, "VCOM", "--");
    s_vcom_query = AddTestButton(lv_obj_get_parent(s_vcom_value), Lang::Strings::SETTINGS_TEST_VCOM_QUERY,
        [](lv_event_t*) {
            if (!PanelVcomRequestRead()) ESP_LOGW(TAG, "VCOM query rejected: service busy or unavailable");
            RefreshAutoInfo(nullptr);
        });
    s_bt_version = AddStatusRow(body, Lang::Strings::SETTINGS_TEST_BT_VERSION, Lang::Strings::BT_CHECKING_VERSION);
    s_bt_query_sent = false;
    s_bt_query_done = false;
    s_bt_query_deadline = esp_timer_get_time() + static_cast<int64_t>(kMetalioAudioInitWaitMs) * 1000;
    RefreshAutoInfo(nullptr);
    s_auto_info_timer = lv_timer_create(RefreshAutoInfo, 250, nullptr);
    if (s_auto_info_timer == nullptr) {
        ESP_LOGE(TAG, "automatic test info timer creation failed");
        if (!s_bt_query_done) lv_label_set_text(s_bt_version, Lang::Strings::COMMON_UNKNOWN);
    }
    SettingsNetworkSignal_Build(body);
    auto* light_value = AddStatusRow(body, Lang::Strings::SETTINGS_FRONTLIGHT_TITLE,
                                   Lang::Strings::SETTINGS_TEST_FRONTLIGHT_START);
    auto* light_row = lv_obj_get_parent(light_value);
    lv_obj_t* light_button = lv_obj_create(light_row);
    lv_obj_remove_style_all(light_button);
    lv_obj_set_size(light_button, UiSx(152), UiSy(40));
    lv_obj_set_style_bg_color(light_button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(light_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(light_button, lv_color_black(), 0);
    lv_obj_set_style_border_width(light_button, kSettingsBorderW, 0);
    lv_obj_set_style_radius(light_button, kSettingsOptionRadius, 0);
    lv_obj_clear_flag(light_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(light_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_parent(light_value, light_button);
    lv_obj_set_flex_grow(light_value, 0);
    lv_obj_set_width(light_value, UiSx(140));
    lv_obj_set_style_text_align(light_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(light_value);
    lv_obj_add_flag(light_row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(light_row);
    lv_obj_add_event_cb(light_row, OnLightTestClicked, LV_EVENT_CLICKED, light_value);
    SettingsTestAudio_BuildRow(body, scr);

    auto& test_ui = SettingsTest_Ui();
    SettingsTest_BeginSession();
    if (MetalioSd_Ok()) QuerySdCapacity();
    test_ui.cell_busy = false;
    test_ui.root_scr = scr;
    test_ui.cell_auto = false;
    test_ui.cell_state = SettingsTestCellState::Idle;
    test_ui.sim_int = SettingsTest_CreateRow(body, Lang::Strings::SETTINGS_TEST_CELL_INT, nullptr);
    test_ui.sim_ext = SettingsTest_CreateRow(body, Lang::Strings::SETTINGS_TEST_CELL_EXT, nullptr);
    test_ui.wifi = SettingsTest_CreateRow(body, "WiFi", SettingsTestWifi_OnRowClicked);
    SettingsTestWifi_ScheduleScan();
    SettingsTestCell_StartAutoSequence();
    SettingsTestCamera_BuildRow(body);
    LayoutAutoTestRows(body);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(container, LV_DIR_VER);

    VkKey_AttachScreen(scr, kAutoScreenId, VkKeyScreenDesc{SettingsTestAutoScreen::Create});
    SettingsTestAudio_OnLoad();
    SettingsTestCamera_OnLoad();
    return scr;
}

