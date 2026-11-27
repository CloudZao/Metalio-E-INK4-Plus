#pragma once
#include <lvgl.h>
#include "sysmgmt/power/standby_presentation.h"

class StandbyScreen {
public:
    /** @brief 创建待机页面根对象，供界面导航使用。 */
    static lv_obj_t* Create();
};
