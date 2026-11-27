#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ui/apps/settings/settings_test/settings_test_wifi.h"
#include "business/diagnostics/wifi_diagnostic.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/settings_test/settings_test_common.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/ui_theme.h"

static WifiDiagnosticRequest s_request;
static bool s_rescan_requested = false;
static UiPageLifecycle s_wifi_lifecycle;
static void RebuildWifiListPage();
static void ScheduleWifiScan();

static UiModal s_window_modal;

static void CloseWifiPopup() {
    lv_obj_t* mask = SettingsTest_Ui().wifi_mask;
    SettingsTest_Ui().wifi_mask = nullptr;
    SettingsTest_Ui().wifi_status = nullptr;
    SettingsTest_Ui().wifi_list = nullptr;
    SettingsTest_Ui().wifi_page_lbl = nullptr;
    SettingsTest_Ui().wifi_rescan_btn = nullptr;
    if (mask != nullptr) {
        lv_obj_delete(mask);
    }
}

static void SetWifiRescanEnabled(bool enabled) {
    if (SettingsTest_Ui().wifi_rescan_btn == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(SettingsTest_Ui().wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(SettingsTest_Ui().wifi_rescan_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(SettingsTest_Ui().wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(SettingsTest_Ui().wifi_rescan_btn, LV_OPA_50, 0);
    }
}

static int WifiPageCount() {
    if (SettingsTest_Ui().wifi_aps.empty()) {
        return 1;
    }
    return static_cast<int>((SettingsTest_Ui().wifi_aps.size() + kSettingsTestWifiPageSize - 1) /
                            kSettingsTestWifiPageSize);
}

static void RebuildWifiListPage() {
    if (!SettingsTest_IsLive() || SettingsTest_Ui().wifi_list == nullptr) {
        return;
    }
    lv_obj_clean(SettingsTest_Ui().wifi_list);

    if (SettingsTest_Ui().wifi_scan_busy) {
        if (SettingsTest_Ui().wifi_status != nullptr) {
            lv_label_set_text(SettingsTest_Ui().wifi_status, LangString_SETTINGS_TEST_WIFI_SCANNING);
        }
        if (SettingsTest_Ui().wifi_page_lbl != nullptr) {
            lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, "");
        }
        return;
    }

    char status[48];
    std::snprintf(status, sizeof(status), LangString_SETTINGS_TEST_WIFI_COUNT_FMT,
                  static_cast<int>(SettingsTest_Ui().wifi_aps.size()));
    if (SettingsTest_Ui().wifi_status != nullptr) {
        lv_label_set_text(SettingsTest_Ui().wifi_status, status);
    }

    const int pages = WifiPageCount();
    if (SettingsTest_Ui().wifi_page < 0) {
        SettingsTest_Ui().wifi_page = 0;
    }
    if (SettingsTest_Ui().wifi_page >= pages) {
        SettingsTest_Ui().wifi_page = pages - 1;
    }

    if (SettingsTest_Ui().wifi_page_lbl != nullptr) {
        char foot[32];
        std::snprintf(foot, sizeof(foot), "%d / %d", SettingsTest_Ui().wifi_page + 1, pages);
        lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, foot);
    }

    if (SettingsTest_Ui().wifi_aps.empty()) {
        lv_obj_t* hint = lv_label_create(SettingsTest_Ui().wifi_list);
        lv_label_set_text(hint, LangString_SETTINGS_TEST_WIFI_NONE);
        lv_obj_set_style_text_font(hint, UiFont(), 0);
        return;
    }

    const int start = SettingsTest_Ui().wifi_page * kSettingsTestWifiPageSize;
    const int end = std::min(start + kSettingsTestWifiPageSize, static_cast<int>(SettingsTest_Ui().wifi_aps.size()));
    for (int i = start; i < end; ++i) {
        const auto& ap = SettingsTest_Ui().wifi_aps[static_cast<size_t>(i)];
        lv_obj_t* item = lv_obj_create(SettingsTest_Ui().wifi_list);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, lv_pct(100));
        lv_obj_set_height(item, 40);
        lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, UiColorForeground(), 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* ssid = lv_label_create(item);
        lv_label_set_text(ssid, ap.ssid.c_str());
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ssid, UiFont(), 0);

        char meta[32];
        std::snprintf(meta, sizeof(meta), "%ddBm", ap.rssi);
        lv_obj_t* ml = lv_label_create(item);
        lv_label_set_text(ml, meta);
        lv_obj_set_style_text_font(ml, UiFont(), 0);
    }
}

static void OnWifiClose(lv_event_t* /*e*/) {
    CloseWifiPopup();
}

static void OnWifiRescan(lv_event_t* /*e*/) {
    ScheduleWifiScan();
}

static void OnWifiPrev(lv_event_t* /*e*/) {
    if (SettingsTest_Ui().wifi_page > 0) {
        --SettingsTest_Ui().wifi_page;
        RebuildWifiListPage();
    }
}

static void OnWifiNext(lv_event_t* /*e*/) {
    if (SettingsTest_Ui().wifi_page + 1 < WifiPageCount()) {
        ++SettingsTest_Ui().wifi_page;
        RebuildWifiListPage();
    }
}

static lv_obj_t* MakePopupBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 88, 44);
    lv_obj_set_style_bg_color(btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(btn, kScreenBorderW, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void OpenWifiPopup() {
    if (SettingsTest_Ui().wifi_mask != nullptr || !SettingsTest_IsLive() || SettingsTest_Ui().root_scr == nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(SettingsTest_Ui().root_scr);
    SettingsTest_Ui().wifi_mask = mask;
    s_window_modal.Bind(mask, [](void*) { CloseWifiPopup(); });
    lv_obj_add_event_cb(
        mask,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) != SettingsTest_Ui().wifi_mask) {
                return;
            }
            // 父页面销毁时只失效引用，由 LVGL 继续删除弹窗及其子控件。
            SettingsTest_Ui().wifi_mask = nullptr;
            CloseWifiPopup();
        },
        LV_EVENT_DELETE, nullptr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(mask);
    lv_obj_add_event_cb(mask, OnWifiClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 32, kSettingsTestWifiPopupH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, LangString_SETTINGS_TEST_WIFI_NEARBY);
    lv_obj_set_style_text_font(title, UiFont(), 0);

    SettingsTest_Ui().wifi_status = lv_label_create(card);
    lv_label_set_text(SettingsTest_Ui().wifi_status, "--");
    lv_obj_set_style_text_font(SettingsTest_Ui().wifi_status, UiFont(), 0);
    lv_obj_set_width(SettingsTest_Ui().wifi_status, lv_pct(100));

    SettingsTest_Ui().wifi_list = lv_obj_create(card);
    lv_obj_remove_style_all(SettingsTest_Ui().wifi_list);
    lv_obj_set_width(SettingsTest_Ui().wifi_list, lv_pct(100));
    lv_obj_set_flex_grow(SettingsTest_Ui().wifi_list, 1);
    lv_obj_set_flex_flow(SettingsTest_Ui().wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(SettingsTest_Ui().wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    SettingsTest_Ui().wifi_page_lbl = lv_label_create(card);
    lv_label_set_text(SettingsTest_Ui().wifi_page_lbl, "");
    lv_obj_set_style_text_font(SettingsTest_Ui().wifi_page_lbl, UiFont(), 0);
    lv_obj_set_style_text_align(SettingsTest_Ui().wifi_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(SettingsTest_Ui().wifi_page_lbl, lv_pct(100));

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 48);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    MakePopupBtn(btn_row, LangString_SETTINGS_TEST_WIFI_PREV, OnWifiPrev);
    SettingsTest_Ui().wifi_rescan_btn = MakePopupBtn(btn_row, LangString_SETTINGS_TEST_WIFI_RESCAN, OnWifiRescan);
    MakePopupBtn(btn_row, LangString_SETTINGS_TEST_WIFI_NEXT, OnWifiNext);
    MakePopupBtn(btn_row, LangString_SETTINGS_TEST_WIFI_CLOSE, OnWifiClose);

    SetWifiRescanEnabled(!SettingsTest_Ui().wifi_scan_busy);
    RebuildWifiListPage();
}

static void OnWifiRowClicked(lv_event_t* /*e*/) {
    OpenWifiPopup();
}

static void PollWifiScan() {
    if (s_rescan_requested && SettingsTest_IsLive() && !WifiDiagnosticIsRunning()) {
        ScheduleWifiScan();
    }
    if (!s_request || !s_request->completed.load(std::memory_order_acquire) || !SettingsTest_IsLive()) {
        return;
    }
    auto result = std::move(s_request);
    auto& ui = SettingsTest_Ui();
    ui.wifi_scan_busy = false;
    ui.wifi_aps = std::move(result->access_points);
    ui.wifi_page = 0;
    if (result->success && !ui.wifi_aps.empty()) {
        char text[32];
        std::snprintf(text, sizeof(text), LangString_SETTINGS_TEST_WIFI_FOUND_FMT, int(ui.wifi_aps.size()));
        SettingsTest_SetRowValue(ui.wifi, text);
        SettingsTest_SetRowStatus(ui.wifi, true);
    } else {
        SettingsTest_SetRowValue(ui.wifi, result->success ? LangString_SETTINGS_TEST_WIFI_NONE
                                                        : LangString_SETTINGS_TEST_WIFI_SCAN_FAIL);
        SettingsTest_SetRowStatus(ui.wifi, false);
    }
    SetWifiRescanEnabled(true);
    RebuildWifiListPage();
}

static void ScheduleWifiScan() {
    if (!SettingsTest_IsLive() || SettingsTest_Ui().wifi_scan_busy) {
        return;
    }
    s_wifi_lifecycle.Bind(SettingsTest_Ui().root_scr, {PollWifiScan, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications) {
            SettingsTestWifi_Teardown();
        } else if (event == UiPagePowerEvent::ResumeApplications) {
            ScheduleWifiScan();
        }
    }, nullptr, 100});
    s_rescan_requested = WifiDiagnosticIsRunning();
    if (s_rescan_requested) {
        return;
    }
    s_request = WifiDiagnosticStart();
    if (!s_request) {
        SettingsTest_SetRowValue(SettingsTest_Ui().wifi, LangString_CLOUD_TASK_FAIL);
        SettingsTest_SetRowStatus(SettingsTest_Ui().wifi, false);
        return;
    }
    SettingsTest_Ui().wifi_scan_busy = true;
    SettingsTest_SetRowValue(SettingsTest_Ui().wifi, LangString_SETTINGS_TEST_SCANNING);
    SetWifiRescanEnabled(false);
    RebuildWifiListPage();
}

void SettingsTestWifi_ScheduleScan() { ScheduleWifiScan(); }

void SettingsTestWifi_Teardown() {
    s_rescan_requested = false;
    WifiDiagnosticCancel(s_request);
    s_request.reset();
    SettingsTest_Ui().wifi_scan_busy = false;
    CloseWifiPopup();
}

void SettingsTestWifi_OnRowClicked(lv_event_t* event) { OnWifiRowClicked(event); }
