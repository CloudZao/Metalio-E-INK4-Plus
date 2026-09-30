#pragma once

#include "lvgl.h"

// WiFi 连接页：扫描周边网络、保存已连接网络并处理重连/清理。
class NetworkScreen {
public:
    /** @brief 创建 WiFi 连接页 */
    static lv_obj_t* Create();
    /** @brief 虚拟键处理；返回 true 表示已消费 */
    static bool OnVkKey(const char* key_name);
};
