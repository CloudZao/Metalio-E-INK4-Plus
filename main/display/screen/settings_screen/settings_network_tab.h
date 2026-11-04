#pragma once

#include "lvgl.h"

// 设置 → 网络：
// - WiFi / 4G 上网切换（切换后重启）
// - 仅在当前为 WiFi 时显示：WiFi 开关与进入配网模式
// - 仅在当前为 4G 时显示：内置卡 / 外置卡切换
void SettingsNetworkTab_Build(lv_obj_t* page);
void SettingsNetworkTab_Reset();

// 供设置壳层日志使用
const char* SettingsNetworkTab_CurrentName();

// 仅读取当前网络模式，不访问 NVS。
bool SettingsNetworkTab_IsWifi();
