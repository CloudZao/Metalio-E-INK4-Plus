#pragma once

#include <string>
#include "lvgl.h"

// 百问 AI 页面：管理会话内容、PPT 状态和按键语义。
class AssistantScreen {
public:
    /** @brief 创建百问页 */
    static lv_obj_t* Create();

    /** @brief 当前服务的应用名称；首页使用 compact 英文缩写 */
    static const char* DisplayName(bool compact = false);

};
