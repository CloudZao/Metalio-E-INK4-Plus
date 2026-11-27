#pragma once

#include <lvgl.h>

/**
 * @brief 创建实时网络信息，控件删除时停止采样；同一时间仅支持一个可见实例
 * @param grouped 为 true 时使用设置分组样式，分别显示信号名称与数值
 */
void SettingsNetworkSignal_Build(lv_obj_t* parent, bool grouped = false);
