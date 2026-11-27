#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "lvgl.h"
#include "business/diagnostics/wifi_diagnostic.h"
#include "ui/common/drawing/ui_scale.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/board/nt26_board.h"

// 设置 → 测试 Tab 内部共享类型与状态（仅 settings_test/ 内使用）。

constexpr const char* kSettingsTestIconPass = "A:ic_app_test_pass.spng";
constexpr const char* kSettingsTestIconFail = "A:ic_app_test_fail.spng";
constexpr lv_coord_t kSettingsTestRowH = UiSy(40);
constexpr lv_coord_t kSettingsTestIconSz = 28;
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

using SettingsTestApItem = WifiDiagnosticAccessPoint;

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
    std::vector<SettingsTestApItem> wifi_aps;
    int wifi_page = 0;

    SettingsTestCellState cell_state = SettingsTestCellState::Idle;
    bool cell_auto = false;
    bool cell_busy = false;
};

/** @brief 获取自动测试页共享状态，仅由该测试功能域使用。 */
SettingsTestUi& SettingsTest_Ui();

/** @brief 查询测试页面是否接受回写，后台完成结果应用前须检查。 */
bool SettingsTest_IsLive();

/** @brief 获取测试页面生命周期代次，用于丢弃旧任务结果。 */
uint32_t SettingsTest_Generation();

/** @brief 推进生命周期代次并允许测试页面回写。 */
void SettingsTest_BeginSession();

/** @brief 禁止测试页面回写并推进代次，须在各子模块清理前调用。 */
void SettingsTest_BeginShutdown();

/** @brief 更新仍存活的测试行通过或失败图标。 */
void SettingsTest_SetRowStatus(SettingsTestRow& row, bool pass);

/** @brief 更新仍存活的测试行内容，空指针按空文案处理。 */
void SettingsTest_SetRowValue(SettingsTestRow& row, const char* text);

/** @brief 创建含标题、结果图标和值标签的测试行，并绑定点击回调。 */
SettingsTestRow SettingsTest_CreateRow(lv_obj_t* parent, const char* title, lv_event_cb_t row_click_cb);

/** @brief 获取当前双网络板实例，类型不匹配时返回空指针。 */
DualNetworkBoard* SettingsTest_GetDualBoard();

/** @brief 获取当前使用的蜂窝网络板实例，类型不匹配时返回空指针。 */
Nt26Board* SettingsTest_GetNt26Board();

/** @brief 查询当前板级配置是否使用蜂窝网络。 */
bool SettingsTest_IsCellNetwork();
