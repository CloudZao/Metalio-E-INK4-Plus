#pragma once

#include "ui/common/drawing/display.h"

/**
 * @brief 创建 EPDiy LVGL 显示并画出欢迎页
 * @return Display*（LvAdapterEpdiy），由 Board 持有
 */
Display* DisplaySetup_Create(void);

/**
 * @brief 标记系统准备完成并刷新顶部状态（无 Display 时为 no-op）
 */
void DisplaySetup_SetSystemReady(Display* display);
