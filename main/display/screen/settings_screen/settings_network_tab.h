#pragma once

#include "lvgl.h"

/** @brief 构建设置→网络页（WiFi 配网入口；本轮无 4G） */
void SettingsNetworkTab_Build(lv_obj_t* page);
/** @brief 重置网络页 UI 状态 */
void SettingsNetworkTab_Reset();
/** @brief 当前网络类型显示名（供壳层日志） */
const char* SettingsNetworkTab_CurrentName();
/** @brief 是否为 WiFi 模式（本轮恒为 true） */
bool SettingsNetworkTab_IsWifi();
