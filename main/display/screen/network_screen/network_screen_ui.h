#pragma once

#include "network_screen_priv.h"

/**
 * @brief 网络页单例状态
 * @note 先清 in-progress 再投递 UI，避免排队中的 Rebuild 仍看到扫描中而盖掉结果列表
 */
NetworkUiState& Network_State();

/** @brief 打开全屏状态遮罩 */
void Network_OpenStatusOverlay(const char* msg);
/** @brief 创建统一样式按钮 */
lv_obj_t* Network_MakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                          void* user_data);
/** @brief 密码输入页是否打开 */
bool Network_IsPasswordPageOpen();
/** @brief 关闭状态遮罩 */
void Network_CloseStatusOverlay();
/** @brief 网络页是否仍存活 */
bool Network_ScreenAlive();
/** @brief 按行高估算一页条数 */
int Network_ComputeListPageSize(lv_coord_t row_h);
/** @brief 设置 Tab 按钮选中样式 */
void Network_StyleTabBtn(lv_obj_t* btn, bool selected);
/** @brief WiFi 认证模式文案 */
const char* Network_AuthLabel(wifi_auth_mode_t mode);
/** @brief 页面卸载：停扫描/释放保网等 */
void Network_OnScreenUnload(lv_event_t* e);
/** @brief 去掉滚动条与滑动惯性 */
void Network_Strip(lv_obj_t* obj);
/**
 * @brief WiFi 名称行样式：可折两行，行高随内容，上下对称内边距，单/两行垂直居中
 */
void Network_StyleWifiNameRow(lv_obj_t* row, lv_obj_t* name);
/** @brief 按当前行高重算附近/已保存每页条数 */
void Network_RefreshPageSizes();
/** @brief 附近列表行高估算：尽量填满刷新按钮上方后再换页 */
lv_coord_t Network_NearbyRowHEstimate();
/** @brief 已保存行高估算：含操作按钮，避免一页挤爆 */
lv_coord_t Network_SavedRowHEstimate();
/** @brief 断开原因文案 */
const char* Network_DisconnectReasonText(uint8_t reason);
/** @brief 密码键盘字库：30@2，失败回退 UI 默认字 */
const lv_font_t* Network_KbFont();
/** @brief 重启倒计时定时器回调 */
void Network_RestartTimerCb(lv_timer_t* timer);
/** @brief 打开重启倒计时遮罩 */
void Network_OpenRestartCountdown(const std::string& headline);
/** @brief 网络页 UI 正文字库 */
const lv_font_t* Network_UiFont();
/** @brief 更新状态遮罩文案（遮罩已打开时） */
void Network_SetStatusOverlayText(const char* msg);
/** @brief 重启任务入口 */
void Network_RebootTask(void* arg);
/** @brief UI 正文字高 */
lv_coord_t Network_UiLineH();
/** @brief 刷新按钮样式（扫描中禁用等） */
void Network_StyleScanBtn(bool enabled);
