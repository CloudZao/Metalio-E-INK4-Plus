#pragma once
#include <lvgl.h>

class SettingsTestDisplayScreen {
public:
    /** @brief 创建显示测试页面，进入期间禁止自动待机。 */
    static lv_obj_t* Create();
};
