#pragma once

#include "lvgl.h"

// 设置 → 测试 → 触摸测试：全屏测点，点图标消失；固定点测完后进入随机点。
class SettingsTestTouchScreen {
public:
    /** @brief 创建触摸测试页 */
    static lv_obj_t* Create();
};
