#pragma once

#include "lvgl.h"

/** @brief 构建传输页 U 盘区（启用按钮、容量与状态提示） */
void CloudUsbPanelBuild(lv_obj_t* page);

/** @brief 释放传输页U盘区及通知订阅，后台请求独立收尾 */
void CloudUsbPanelReset();
