#pragma once

#include "lvgl.h"

// 设置 → 测试 → 电池测试：电量计 / 充电芯片实时诊断。
class SettingsTestBatteryScreen {
public:
    /** @brief 创建电池测试页 */
    static lv_obj_t* Create();
};

/** @brief 进待机：停电池轮询定时器 */
void SettingsTestBatteryScreen_PauseForStandby();
/** @brief 退出待机：恢复电池轮询 */
void SettingsTestBatteryScreen_ResumeFromStandby();
