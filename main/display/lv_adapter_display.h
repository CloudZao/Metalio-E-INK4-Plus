#pragma once

#include "lv_adapter_epdiy.h"
#include <lvgl.h>

inline constexpr lv_obj_flag_t kTouchUiPagedVerticalDrag = LV_OBJ_FLAG_USER_2; // 分页手势独占 USER_2；USER_1 已由震动绑定占用

/** @brief 屏内手指是否仍按着（盖板键不算；对齐 397） */
bool TouchUiFingerIsDown();

/**
 * @brief 是否还有未交付的 down/up 边沿（含刷屏期间短按的 press→release 回放）
 */
bool TouchUiHasPendingEdges();

/** @brief 兼容 397 头名 */
using LVAdapterDisplay = LvAdapterEpdiy;
