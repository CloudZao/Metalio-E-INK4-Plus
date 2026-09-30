#pragma once

#include "lvgl.h"

/** @brief 构建设置→存储页（容量与模拟 U 盘） */
void SettingsStorageTab_Build(lv_obj_t* page);
/** @brief 重置存储页并关闭虚拟 U 盘 UI 通知 */
void SettingsStorageTab_Reset();
