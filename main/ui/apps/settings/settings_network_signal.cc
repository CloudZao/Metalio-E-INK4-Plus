#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include "ui/apps/settings/settings_network_signal.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "business/network/network_settings.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr uint32_t kSignalPollMs = 2000;  // 采样周期；4G 查询需占用 AT 通道
static constexpr size_t kSignalTextMax = 48;
static constexpr size_t kIccidTextMax = 32;  // ICCID 号码及占位文本的显示缓冲

static lv_obj_t* s_signal_value = nullptr;
static lv_obj_t* s_iccid_value = nullptr;
static bool s_grouped = false; // 设置页使用分组信息行，自动测试保留测试行样式
static UiPageLifecycle s_page;
static std::shared_ptr<const NetworkSignalResult> s_request;
static uint32_t s_last_sample_ms = 0;
static char s_last_signal_text[kSignalTextMax] = {};
static char s_last_iccid_text[kIccidTextMax] = {};

static bool IsCellularMode() {
    return NetworkSettingsSupportsSim();
}

static void InvalidateSignalSession() {
    s_page.Unbind();
    s_request.reset();
    s_signal_value = nullptr;
    s_iccid_value = nullptr;
    s_grouped = false;
    s_last_signal_text[0] = '\0';
    s_last_iccid_text[0] = '\0';
}

static void KickSignalSample() {
    if (!s_request) {
        s_request = NetworkSettingsSampleSignal();
        s_last_sample_ms = lv_tick_get();
    }
}

static void PollNetworkSignal() {
    if (s_request && s_request->completed.load(std::memory_order_acquire)) {
        auto result = std::move(s_request);
        char text[kSignalTextMax];
        const char* format = LangString_SETTINGS_TEST_SIGNAL_BOARD_UNSUP;
        switch (result->kind) {
            case NetworkSignalKind::Unsupported:
                break;
            case NetworkSignalKind::WifiDisconnected:
                format = LangString_SETTINGS_TEST_SIGNAL_WIFI_DISCONNECTED;
                break;
            case NetworkSignalKind::WifiReadFailed:
                format = LangString_SETTINGS_TEST_SIGNAL_WIFI_READ_FAIL;
                break;
            case NetworkSignalKind::WifiSignal:
                format = LangString_SETTINGS_TEST_SIGNAL_WIFI_RSSI_FMT;
                break;
            case NetworkSignalKind::CellUnavailable:
                format = LangString_SETTINGS_TEST_SIGNAL_CELL_UNAVAIL;
                break;
            case NetworkSignalKind::CellUnknown:
                format = LangString_SETTINGS_TEST_SIGNAL_CELL_CSQ_UNKNOWN;
                break;
            case NetworkSignalKind::CellSignal:
                format = LangString_SETTINGS_TEST_SIGNAL_CELL_CSQ_FMT;
                break;
        }
        if (s_grouped) {
            if (result->kind == NetworkSignalKind::WifiSignal) {
                format = "%d dBm";
            } else if (result->kind == NetworkSignalKind::CellSignal) {
                format = "%d";
            } else if (result->kind == NetworkSignalKind::WifiDisconnected) {
                format = std::strcmp(LangCode, "zh-CN") == 0 ? "未连接" : "Not connected";
            } else {
                format = "--";
            }
        }
        std::snprintf(text, sizeof(text), format, result->strength);
        if (std::strcmp(s_last_signal_text, text) != 0) {
            lv_label_set_text(s_signal_value, text);
            std::snprintf(s_last_signal_text, sizeof(s_last_signal_text), "%s", text);
        }
        if (s_iccid_value) {
            const char* iccid = result->iccid[0] ? result->iccid : LangString_SETTINGS_TEST_SIGNAL_ICCID_UNKNOWN;
            if (std::strcmp(s_last_iccid_text, iccid) != 0) {
                lv_label_set_text(s_iccid_value, iccid);
                std::snprintf(s_last_iccid_text, sizeof(s_last_iccid_text), "%s", iccid);
            }
        }
    }
    if (!s_request && lv_tick_get() - s_last_sample_ms >= kSignalPollMs) {
        KickSignalSample();
    }
}

static lv_obj_t* CreateInfoRow(lv_obj_t* page, const char* title_text, const char* value_text) {
    if (s_grouped) {
        auto* row = SettingsCreateListRow(page, title_text, value_text);
        return lv_obj_get_child(row, 1);
    }
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, UiSy(4), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(title, lv_pct(54), 0);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, value_text);
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, UiFont(), 0);
    lv_obj_set_style_text_color(value, UiColorForeground(), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(value, LV_OBJ_FLAG_CLICKABLE);
    return value;
}

void SettingsNetworkSignal_Build(lv_obj_t* parent, bool grouped) {
    InvalidateSignalSession();
    s_grouped = grouped;
    const bool cellular = IsCellularMode();
    const char* title = LangString_SETTINGS_TEST_SIGNAL_TITLE;
    if (grouped) {
        title = cellular ? (std::strcmp(LangCode, "zh-CN") == 0 ? "4G 信号" : "4G signal")
                         : (std::strcmp(LangCode, "zh-CN") == 0 ? "Wi-Fi 信号" : "Wi-Fi signal");
    }
    auto* rows = grouped ? SettingsCreateListGroup(parent) : parent;
    s_signal_value = CreateInfoRow(rows, title, LangString_SETTINGS_TEST_SIGNAL_READING);
    if (cellular) {
        s_iccid_value = CreateInfoRow(rows, grouped ? "ICCID" : LangString_SETTINGS_TEST_SIGNAL_ICCID_TITLE,
                                      LangString_SETTINGS_TEST_SIGNAL_ICCID_UNKNOWN);
    }
    lv_obj_add_event_cb(
        s_signal_value,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == s_signal_value) {
                InvalidateSignalSession();
            }
        },
        LV_EVENT_DELETE, nullptr);
    s_page.Bind(parent, {PollNetworkSignal, nullptr, InvalidateSignalSession, 100}, true);
    KickSignalSample();
}
