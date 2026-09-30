#pragma once

#include "lvgl.h"

/** @brief 构建设置→关于页（系统信息列表；本轮无 OTA） */
void SettingsAboutTab_Build(lv_obj_t* page);
/** @brief 重置关于页 */
void SettingsAboutTab_Reset();
