#pragma once

#include "lvgl.h"

// 外置蓝牙模块的控件、设备列表与交互呈现。
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

    /** @brief 逐级收起名称弹窗、键盘或设备管理页；已处理返回 true */
    static bool DismissOverlay();

};
