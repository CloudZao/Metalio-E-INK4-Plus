#pragma once

#include "network_screen/network_screen.h"

#include "screen_common.h"
#include "ui_scale.h"

#include <cstdint>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lvgl.h>

constexpr const char* TAG = "NetworkScreen";
constexpr const char* kScreenId = "network";

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;
constexpr int kPageSizeFallback = 7;
constexpr int kRestartCountdownSec = 3;

// 源值对齐 397 network_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kBodyPad = UiSx(12);
constexpr lv_coord_t kBodyRowGap = UiSy(8);
constexpr lv_coord_t kTabRowH = UiSy(48);
constexpr lv_coord_t kTabGap = UiSx(8);
constexpr lv_coord_t kTabBtnW = UiSx(120);
constexpr lv_coord_t kTabBtnH = UiSy(44);
constexpr lv_coord_t kFootRowGap = UiSy(6);
constexpr lv_coord_t kActionRowH = UiSy(44);
constexpr lv_coord_t kActionBtnW = UiSx(100);
constexpr lv_coord_t kActionBtnH = UiSy(40);
constexpr lv_coord_t kOverlayCardMargin = UiSx(48);
constexpr lv_coord_t kOverlayCardH = UiSy(160);
constexpr lv_coord_t kOverlayCardRadius = UiSx(12);
constexpr lv_coord_t kOverlayCardPad = UiSx(16);
constexpr lv_coord_t kOverlayBorderW = UiSx(2);
constexpr lv_coord_t kBtnRadius = UiSx(6);
constexpr lv_coord_t kBtnBorderW = UiSx(2);
constexpr lv_coord_t kRowPadHor = UiSx(4);
constexpr lv_coord_t kRowPadVer = UiSy(6);  // SSID 与底部分割线留缝；单行由对称上下 padding 保证垂直居中
constexpr lv_coord_t kSavedActionBtnH = UiSy(40);  // 已保存行「设默认/删除」按钮高
constexpr lv_coord_t kSavedDefBtnW = UiSx(88);
constexpr lv_coord_t kSavedDelBtnW = UiSx(72);
constexpr lv_coord_t kPwdPagePad = UiSx(12);
constexpr lv_coord_t kPwdPageRowGap = UiSy(10);
constexpr lv_coord_t kPwdBoxH = UiSy(56);
constexpr lv_coord_t kPwdBoxPadH = UiSx(10);
constexpr lv_coord_t kPwdBoxPadV = UiSy(10);
constexpr lv_coord_t kPwdBtnRowH = UiSy(48);
constexpr lv_coord_t kPwdBtnW = UiSx(140);
constexpr lv_coord_t kPwdBtnH = UiSy(44);

constexpr EventBits_t kBitScanDone = BIT0;
constexpr EventBits_t kBitConnected = BIT1;
constexpr EventBits_t kBitDisconnected = BIT2;

enum class Tab : uint8_t { kNearby = 0, kSaved = 1 };

struct ApItem {
    std::string ssid;
    int8_t rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

struct UiWidgets {
    lv_obj_t* screen = nullptr;
    lv_obj_t* list_panel = nullptr;  // 扫网/已保存主面板
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* nearby_tab_btn = nullptr;
    lv_obj_t* saved_tab_btn = nullptr;
    lv_obj_t* scan_btn = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* clear_btn = nullptr;
    // 密码页（全屏，非弹框）：上 SSID+输入，下键盘
    lv_obj_t* pwd_page = nullptr;
    lv_obj_t* pwd_ssid_lbl = nullptr;
    lv_obj_t* pwd_box = nullptr;   // 输入框边框容器
    lv_obj_t* pwd_lbl = nullptr;   // 与拨号页一致：label 显示，避免 textarea 重
    lv_obj_t* pwd_keyboard = nullptr;
    lv_obj_t* status_overlay = nullptr;
    lv_obj_t* status_msg = nullptr;
};

enum class KbMode : uint8_t { kEn = 0, kSym = 1 };
constexpr int kKbCols = 6;
constexpr int kKbRows = 7;  // 6 行字符 + 1 行功能（英需 10 数字 + 26 字母）
constexpr lv_coord_t kKbGap = UiSx(4);

enum class KbAction : uint8_t {
    kChar = 0,
    kBackspace,
    kShift,
    kModeEn,
    kModeSym,
    kSpace,
    kEmpty,
};

struct KbKeyDesc {
    const char* label = nullptr;  // 显示
    const char* insert = nullptr; // 插入文本；功能键可为 nullptr
    KbAction action = KbAction::kChar;
};

struct NearbyClickCtx {
    char ssid[kMaxSsidLen + 1] = {};
    int authmode = 0;
};

struct SavedActionCtx {
    int index = 0;
};

struct ConnectCtx {
    std::string ssid;
    std::string password;
};

struct AsyncStatusMsg {
    char text[160] = {};
};

struct AsyncFailMsg {
    std::string title;
    std::string detail;
};

struct NetworkUiState {
    UiWidgets ui{};
    std::vector<ApItem> scan_results;
    bool screen_active = false;
    bool wifi_initialized = false;
    bool ui_net_held = false;
    bool wifi_station_was_active = false;
    bool scan_in_progress = false;
    bool connect_in_progress = false;
    Tab tab = Tab::kNearby;
    int nearby_page = 0;
    int saved_page = 0;
    int nearby_page_size = kPageSizeFallback;
    int saved_page_size = kPageSizeFallback;
    std::string pending_ssid;
    wifi_auth_mode_t pending_authmode = WIFI_AUTH_OPEN;
    KbMode kb_mode = KbMode::kEn;
    bool kb_upper = false;
    lv_obj_t* kb_keys[kKbRows * kKbCols] = {};
    char password[kMaxPasswordLen + 1] = {};
    esp_netif_t* netif = nullptr;
    esp_event_handler_instance_t wifi_evt_inst = nullptr;
    esp_event_handler_instance_t ip_evt_inst = nullptr;
    EventGroupHandle_t evt_group = nullptr;
    uint8_t last_disconnect_reason = 0;
    lv_timer_t* restart_timer = nullptr;
    lv_timer_t* fail_close_timer = nullptr;
    int restart_remaining = 0;
    std::string restart_headline;
};

/** @brief 网络屏 UI 单例（先清 in-progress，再投递 UI） */
NetworkUiState& Network_State();

/** @brief 重建当前列表页 */
void Network_RebuildListPage();
/** @brief 进入后自动扫一次 */
void Network_ScheduleScan();
/** @brief 调度连接指定 SSID */
void Network_ScheduleConnect(const std::string& ssid, const std::string& password);
/** @brief 打开密码页 */
void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode);
/** @brief 关闭密码页 */
void Network_ClosePasswordPage();
/** @brief 关闭状态遮罩 */
void Network_CloseStatusOverlay();
/** @brief 打开状态遮罩 */
void Network_OpenStatusOverlay(const char* msg);
/** @brief 打开重启倒计时 */
void Network_OpenRestartCountdown(const std::string& headline);
/** @brief 设置 Tab 按钮选中样式 */
void Network_StyleTabBtn(lv_obj_t* btn, bool selected);
/** @brief 设置扫描按钮可用样式 */
void Network_StyleScanBtn(bool enabled);
/** @brief 网络屏 UI 字体 */
const lv_font_t* Network_UiFont();
/** @brief 键盘用系统字库 30@2，失败回退 UI 默认字 */
const lv_font_t* Network_KbFont();
/** @brief 网络屏是否仍存活 */
bool Network_ScreenAlive();
/** @brief WiFi 认证模式文案 */
const char* Network_AuthLabel(wifi_auth_mode_t mode);
/** @brief 断开原因文案 */
const char* Network_DisconnectReasonText(uint8_t reason);
/** @brief 去掉对象装饰边框/阴影 */
void Network_Strip(lv_obj_t* obj);
/** @brief 名称可折两行：行高随内容，上下对称内边距避免贴分割线，同时保证单/两行垂直居中 */
void Network_StyleWifiNameRow(lv_obj_t* row, lv_obj_t* name);
/** @brief 创建统一风格按钮 */
lv_obj_t* Network_MakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                  void* user_data = nullptr);
/** @brief 投递状态文案到 UI 线程 */
void Network_PostStatus(const char* text);
/** @brief 投递重建列表到 UI 线程 */
void Network_PostRebuildList();
/** @brief 投递扫描按钮可用状态 */
void Network_PostScanBtnEnabled(bool enabled);
/** @brief 挡 AppIdle（不跑保网断网）；待机计时仍走（Evaluate 先判待机再判 SoftKeepNet） */
void Network_AcquireUiKeepNet();
/** @brief 释放 UI 保网 */
void Network_ReleaseUiKeepNet();
/** @brief 退屏时拆除 WiFi 扫描/连接态 */
void Network_WifiTeardownForScreen();
/** @brief 附近网络总页数 */
int Network_NearbyPageCount();
/** @brief 已保存网络总页数 */
int Network_SavedPageCount();
/** @brief 构建密码输入页 */
void Network_BuildPasswordPage(lv_obj_t* parent, lv_coord_t top_y);
