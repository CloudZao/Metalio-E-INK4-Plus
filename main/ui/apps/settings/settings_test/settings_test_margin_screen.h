#pragma once

#include <lvgl.h>

class SettingsTestMarginScreen {
public:
    /** @brief 创建四边边距调节与可显示区域预览页 */
    static lv_obj_t* Create();

    /** @brief 在待机底图捕获前暂停边距预览，唤醒后恢复 */
    static void SetStandby(bool standby);
};
