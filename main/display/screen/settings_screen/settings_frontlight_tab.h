#pragma once

#include "lvgl.h"

/** @brief 构建设置→屏幕页的刷新模式及黑色深度选项 */
void SettingsFrontlightTab_Build(lv_obj_t* page);
/** @brief 重置屏幕设置页 UI 指针 */
void SettingsFrontlightTab_Reset();
/** @brief 同步屏幕刷新配置控件 */
void SettingsFrontlightTab_Refresh();
