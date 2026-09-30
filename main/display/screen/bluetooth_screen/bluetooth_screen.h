#pragma once

#include "lvgl.h"

// 外置蓝牙音频模块控制页，使用 SimpleUart 与 BT 芯片交互。
class BluetoothScreen {
public:
    /** @brief 把蓝牙页控件构建进 parent */
    static void BuildInto(lv_obj_t* parent);
    /** @brief 复位蓝牙页 UI 状态 */
    static void ResetUi();
    /** @brief Tab/页激活时启动轮询或刷新 */
    static void OnActivated();
    /** @brief Tab/页停用时停止轮询 */
    static void OnDeactivated();
    /** @brief 开机默认应用模式 1，适用于 UART 初始化后、UI 未启动时场景。 */
    static void ApplyDefaultMode();
};
