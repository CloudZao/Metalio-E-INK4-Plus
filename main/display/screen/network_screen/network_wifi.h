#pragma once

#include "network_screen_priv.h"

/** @brief 投递连接失败对话框 */
void Network_PostShowFailure(const std::string& title, const std::string& detail);
/** @brief WiFi 扫描任务入口 */
void Network_ScanTask(void* arg);
/** @brief WiFi 系统事件处理 */
void Network_WifiEvtHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
/**
 * @brief 挡 AppIdle（不跑保网断网）；待机计时仍走（Evaluate 先判待机再判 SoftKeepNet）
 */
void Network_AcquireUiKeepNet();
/** @brief WiFi 连接任务入口 */
void Network_ConnectTask(void* arg);
/** @brief 进入页面后自动扫一次 */
void Network_ScheduleScan();
/** @brief 投递连接成功页 */
void Network_PostOpenSuccess(const std::string& ssid);
/** @brief 为网络页初始化 WiFi STA */
bool Network_WifiInitForScreen();
/** @brief 释放 UI 保网挡板 */
void Network_ReleaseUiKeepNet();
/** @brief 异步打开连接成功页 */
void Network_AsyncOpenSuccess(void* p);
/** @brief 调度连接指定 SSID */
void Network_ScheduleConnect(const std::string& ssid, const std::string& password);
/** @brief 离开网络页时拆除 WiFi 扫描/连接资源 */
void Network_WifiTeardownForScreen();
/** @brief 异步弹出连接失败 */
void Network_AsyncShowFailure(void* p);
