#pragma once

#include "lvgl.h"

// 云传输页面：展示书籍、壁纸和字体资源，支持下载、删除和停止传输。
class CloudScreen {
public:
    /** @brief 创建云传输页 */
    static lv_obj_t* Create();
    /** @brief 停止当前传输并收起进度 UI；可从多线程调用 */
    static void StopTransfer();
};
