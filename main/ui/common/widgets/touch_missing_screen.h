#pragma once

#include "lvgl.h"

// 触摸 IC 检测失败时的提示页（全屏居中文案）。
class TouchMissingScreen {
public:
    /** @brief 创建触摸 IC 缺失提示页 */
    static lv_obj_t* Create();
};
