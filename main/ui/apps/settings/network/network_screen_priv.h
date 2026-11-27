#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "business/transfer/ap_transfer.h"
#include "business/network/wifi_setup.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/common/drawing/ui_scale.h"

constexpr const char* TAG = "NetworkScreen";
constexpr const char* kScreenId = "network";

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;
constexpr int kPageSizeFallback = 7;

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
constexpr lv_coord_t kOverlayCardMargin = UiSx(80);
constexpr lv_coord_t kOverlayCardMinH = UiSy(96);
constexpr lv_coord_t kOverlayCardPad = UiSx(16);
constexpr lv_coord_t kOverlayBorderW = UiSx(2);
constexpr lv_coord_t kBtnRadius = UiSx(6);
constexpr lv_coord_t kBtnBorderW = UiSx(2);
constexpr lv_coord_t kRowPadHor = UiSx(4);
constexpr lv_coord_t kRowPadVer = UiSy(6);         // SSID 与底部分割线留缝；单行由对称上下 padding 保证垂直居中
constexpr lv_coord_t kSavedActionBtnH = UiSy(40);  // 已保存行「设默认/删除」按钮高
constexpr lv_coord_t kSavedDefBtnW = UiSx(88);
constexpr lv_coord_t kSavedDelBtnW = UiSx(72);
constexpr lv_coord_t kPwdPagePad = UiSx(12);
constexpr lv_coord_t kPwdPageRowGap = UiSy(10);
constexpr lv_coord_t kPwdBoxH = UiSy(64);
constexpr lv_coord_t kPwdBoxPadH = UiSx(10);
constexpr lv_coord_t kPwdBoxPadV = UiSy(10);
constexpr lv_coord_t kPwdBtnRowH = UiSy(48);
constexpr lv_coord_t kPwdBtnW = UiSx(140);
constexpr lv_coord_t kPwdBtnH = UiSy(44);


enum class Tab : uint8_t { kNearby = 0, kSaved = 1 };

using ApItem = WifiSetupAccessPoint;

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
    lv_obj_t* pwd_box = nullptr;  // 输入框边框容器
    lv_obj_t* pwd_lbl = nullptr;  // 与拨号页一致：label 显示，避免 textarea 重
    lv_obj_t* pwd_keyboard = nullptr;
    lv_obj_t* status_overlay = nullptr;
    lv_obj_t* status_msg = nullptr;
};

struct NearbyClickCtx {
    char ssid[kMaxSsidLen + 1] = {};
    int authmode = 0;
};

struct SavedActionCtx {
    std::string ssid;  // 按网络名称定位，避免列表重排后下标失效
};

struct NetworkStatusMessage {
    uint32_t generation = 0;  // 发起页面的代次
    char text[160] = {};  // 状态文案
};

struct NetworkUiState {
    std::atomic<uint32_t> generation{0};  // 过滤页面切换前排队的异步结果
    UiWidgets ui{};
    std::vector<ApItem> scan_results;
    bool settings_mode = false;  // 设置页复用扫描和连接逻辑，保持文件服务关闭
    bool settings_detail = false;  // 正在查看网络详情，扫描结果不替换详情
    bool settings_list_dirty = false;  // 手指释放后再更新扫描列表
    bool transfer_mode = false;      // 连接后提供局域网文件传输
    ApTransferInfo transfer_info{};  // 已启动服务的网址
    std::atomic<bool> screen_active{false};
    std::atomic<bool> scan_in_progress{false};
    std::atomic<bool> connect_in_progress{false};
    Tab tab = Tab::kNearby;
    int nearby_page = 0;
    int saved_page = 0;
    bool transfer_scan_pending = false;         // 扫描结果提示结束前暂不显示列表
    std::string transfer_scan_message;          // 居中显示的扫描结果提示
    lv_timer_t* transfer_scan_timer = nullptr;  // 扫描结果提示结束后展开列表
    int nearby_page_size = kPageSizeFallback;
    int saved_page_size = kPageSizeFallback;
    std::string retry_ssid;  // 连接失败后允许重新输入已保存网络的密码
    std::string pending_ssid;
    wifi_auth_mode_t pending_authmode = WIFI_AUTH_OPEN;
    char password[kMaxPasswordLen + 1] = {};
    lv_timer_t* fail_close_timer = nullptr;
    std::atomic<bool> restart_pending{false};
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


/** @brief 设置 Tab 按钮选中样式 */
void Network_StyleTabBtn(lv_obj_t* btn, bool selected);

/** @brief 设置扫描按钮可用样式 */
void Network_StyleScanBtn(bool enabled);

/** @brief 网络屏 UI 字体 */
const lv_font_t* Network_UiFont();

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

/** @brief 显示扫描结果提示，结束后展开 WIFI 分组列表 */
void Network_PostScanResult(const char* text);

/** @brief 投递重建列表到 UI 线程 */
void Network_PostRebuildList();

/** @brief 附近网络总页数 */
int Network_NearbyPageCount();

/** @brief 已保存网络总页数 */
int Network_SavedPageCount();

/** @brief 构建密码输入页 */
void Network_BuildPasswordPage(lv_obj_t* parent, lv_coord_t top_y);

/** @brief 页面卸载时解除接收并请求后台关闭。 */
void Network_OnScreenUnload(lv_event_t* event);

/** @brief 按行高计算当前列表的页容量。 */
int Network_ComputeListPageSize(lv_coord_t row_height);

/** @brief 按当前布局刷新附近及已保存列表的页容量。 */
void Network_RefreshPageSizes();

/** @brief 获取附近网络行的预计高度。 */
lv_coord_t Network_NearbyRowHEstimate();

/** @brief 获取已保存网络行的预计高度。 */
lv_coord_t Network_SavedRowHEstimate();

/** @brief 更新状态遮罩的提示内容。 */
void Network_SetStatusOverlayText(const char* text);

/** @brief 获取网络页面的正文字高。 */
lv_coord_t Network_UiLineH();

/** @brief 返回 Wi-Fi 列表，详情页关闭后返回 true。 */
bool Network_SettingsBack();
