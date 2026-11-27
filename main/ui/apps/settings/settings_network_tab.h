#pragma once

#include "lvgl.h"

/** @brief 创建网络设置页，按当前 WiFi 或蜂窝网络显示对应配置。 */
void SettingsNetworkTab_Build(lv_obj_t* page);

/** @brief 构建 Wi-Fi 开关行并订阅现有网络开关状态。 */
void SettingsNetworkTab_BuildWifi(lv_obj_t* page);

/** @brief 关闭浮层并解除页面更新与结果接收，网络操作独立完成。 */
void SettingsNetworkTab_Reset();

/** @brief 获取当前网络模式的名称，返回 WiFi 或 4G。 */
const char* SettingsNetworkTab_CurrentName();
