#pragma once
#include <lvgl.h>

/** @brief 创建绑定面板，绑定完成后在 UI 线程通知页面 */
lv_obj_t* YunxinBindingCreatePanel(lv_obj_t* parent, void (*on_bound)() = nullptr);
