#pragma once

#include "lvgl.h"

class PagerScreen {
public:
    /** @brief 创建独立的翻页器设备管理页面 */
    static lv_obj_t* Create();
};
