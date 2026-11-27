#pragma once

#include "lvgl.h"

class SettingsTestAutoScreen {
public:
    /** @brief 创建网络、相机及音频等外设自动测试页面。 */
    static lv_obj_t* Create();
};

class SettingsTestAgingScreen {
public:

    /** @brief 创建重启后的老化页面，并恢复返回测试设置的导航层级。 */
    static lv_obj_t* CreateBootResume();


    /** @brief 创建老化测试页面并启动循环测试，离页时释放测试资源。 */
    static lv_obj_t* Create();

};
