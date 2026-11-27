#pragma once

#include "lvgl.h"

/** @brief 构建前光页的模式及亮度控件。 */
void SettingsFrontlightTab_Build(lv_obj_t* page);

/** @brief 构建屏幕页的刷新模式及 VCOM 或黑色深度控件。 */
void SettingsDisplayTab_Build(lv_obj_t* page);

/** @brief 重置屏幕设置页 UI 指针 */
void SettingsFrontlightTab_Reset();

/** @brief 同步屏幕刷新及前光配置控件 */
void SettingsFrontlightTab_Refresh();
