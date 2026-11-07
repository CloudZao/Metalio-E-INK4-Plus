#pragma once

#include <lvgl.h>

/** @brief 构建系统设置中的翻页器页面 */
void SettingsPagerTab_Build(lv_obj_t* page);
/** @brief 释放翻页器页面的 UI 引用，保留已建立的 BLE 连接 */
void SettingsPagerTab_Reset();
