#pragma once

#include "lvgl.h"

/** @brief 构建设置→功耗页（空闲降频 / 保网 / 浅睡待机 / 自动关机） */
void SettingsPowerTab_Build(lv_obj_t* page);
/** @brief 重置功耗页 UI 指针 */
void SettingsPowerTab_Reset();
