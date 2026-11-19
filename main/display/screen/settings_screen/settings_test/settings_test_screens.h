#pragma once

#include "lvgl.h"

class SettingsTestAutoScreen {
public:
    static lv_obj_t* Create();
};

class SettingsTestAgingScreen {
public:
    /** @brief 在内部 RAM 栈的启动任务读取续跑状态，返回是否需要恢复老化 */
    static bool LoadBootState();
    /** @brief 创建重启后的老化页面，并恢复返回测试设置的导航层级 */
    static lv_obj_t* CreateBootResume();
    static bool IsActive();
    static lv_obj_t* Create();
};
