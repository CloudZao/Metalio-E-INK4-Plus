#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lvgl.h>
#include "dual_network_board.h"
#include "nt26_board.h"
#include "ui_scale.h"

// 设置 → 测试 Tab 内部共享类型与状态（仅 settings_test/ 内使用）。

constexpr const char* kSettingsTestIconPass = "A:ic_app_test_pass.spng";
constexpr const char* kSettingsTestIconFail = "A:ic_app_test_fail.spng";
constexpr lv_coord_t kSettingsTestRowH = UiSy(40);
constexpr lv_coord_t kSettingsTestIconSz = 28;
constexpr lv_coord_t kSettingsTestTitleW = 152;  // 容纳 4G·内置等单行名称
constexpr int kSettingsTestSimExternal = 0;
constexpr int kSettingsTestSimInternal = 1;
constexpr uint32_t kSettingsTestCellWaitPollMs = 500;
constexpr uint32_t kSettingsTestCellWaitMaxMs = 30000;
constexpr uint32_t kSettingsTestBitScanDone = 1u;
constexpr int kSettingsTestWifiPageSize = 4;
constexpr lv_coord_t kSettingsTestWifiPopupH = 380;

struct SettingsTestRow {
    lv_obj_t* row = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* value = nullptr;
};

struct SettingsTestApItem {
    std::string ssid;
    int8_t rssi = -127;
};

enum class SettingsTestCellState {
    Idle,
    TestingInternal,
    TestingExternal,
};

struct SettingsTestUi {
    lv_obj_t* root_scr = nullptr;
    SettingsTestRow wifi;
    SettingsTestRow camera;
    SettingsTestRow sim_int;
    SettingsTestRow sim_ext;

    lv_obj_t* wifi_mask = nullptr;
    lv_obj_t* wifi_status = nullptr;
    lv_obj_t* wifi_list = nullptr;
    lv_obj_t* wifi_page_lbl = nullptr;
    lv_obj_t* wifi_rescan_btn = nullptr;

    lv_timer_t* cell_start_timer = nullptr;
    lv_timer_t* cell_retry_timer = nullptr;

    bool wifi_scan_busy = false;
    bool wifi_stack_owned = false;
    std::vector<SettingsTestApItem> wifi_aps;
    int wifi_page = 0;
    esp_netif_t* wifi_netif = nullptr;

    SettingsTestCellState cell_state = SettingsTestCellState::Idle;
    bool cell_auto = false;
    bool cell_busy = false;
    uint32_t last_cfun_ms = 0;
};

SettingsTestUi& SettingsTest_Ui();

// 页内后台任务 / lv_async_call 回写 UI 前必须检查；离页时先 BeginShutdown 再 Teardown。
bool SettingsTest_IsLive();
uint32_t SettingsTest_Generation();
void SettingsTest_BeginSession();
void SettingsTest_BeginShutdown();

void SettingsTest_SetRowStatus(SettingsTestRow& row, bool pass);
void SettingsTest_SetRowValue(SettingsTestRow& row, const char* text);
SettingsTestRow SettingsTest_CreateRow(lv_obj_t* parent, const char* title,
                                       lv_event_cb_t row_click_cb);

DualNetworkBoard* SettingsTest_GetDualBoard();
Nt26Board* SettingsTest_GetNt26Board();
bool SettingsTest_IsCellNetwork();
