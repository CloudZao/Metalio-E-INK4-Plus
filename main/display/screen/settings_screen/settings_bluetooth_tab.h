#pragma once

#include "lvgl.h"

/** @brief 构建设置→蓝牙 Tab 内容 */
void SettingsBluetoothTab_Build(lv_obj_t* page);
/** @brief 重置蓝牙 Tab UI 状态 */
void SettingsBluetoothTab_Reset();
/** @brief 蓝牙 Tab 激活（开始轮询/刷新） */
void SettingsBluetoothTab_OnActivated();
/** @brief 蓝牙 Tab 停用（停止轮询） */
void SettingsBluetoothTab_OnDeactivated();
