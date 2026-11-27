#pragma once
#include "lvgl.h"

/** @brief 创建相机检测行及拍照入口。 */
void SettingsTestCamera_BuildRow(lv_obj_t* parent);

/** @brief 进入测试页时启动相机检测和结果轮询。 */
void SettingsTestCamera_OnLoad();

/** @brief 测试会话停止后回收相机结果、预览浮层和轮询定时器。 */
void SettingsTestCamera_Teardown();
