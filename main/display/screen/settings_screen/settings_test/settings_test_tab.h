#pragma once

#include "lvgl.h"

/** @brief 构建设置→测试入口页 */
void SettingsTestTab_Build(lv_obj_t* page);
/** @brief 重置测试入口页 */
void SettingsTestTab_Reset();
/** @brief 测试 Tab 激活 */
void SettingsTestTab_OnActivated();
/** @brief 测试 Tab 停用 */
void SettingsTestTab_OnDeactivated();
