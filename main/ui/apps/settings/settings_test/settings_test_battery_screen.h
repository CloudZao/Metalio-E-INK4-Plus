#pragma once

#include "lvgl.h"

// 设置 → 测试 → 电池测试：电量计 / 充电芯片实时诊断。
class SettingsTestBatteryScreen {
public:
    /** @brief 创建电池测试页 */
    static lv_obj_t* Create();
};
