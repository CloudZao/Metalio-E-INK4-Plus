#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include "ui/common/drawing/display.h"

/**
 * @brief 创建 LVAdapterDisplay，并在触摸可用时注册盖板虚拟键
 * @param panel SSD1677 panel
 * @param panel_io panel IO
 * @param touch 触摸句柄（可空）
 * @return Display*（LVAdapterDisplay），调用方持有
 */
Display* DisplaySetup_Create(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_touch_handle_t touch);

/**
 * @brief 触摸自睡唤醒后踢一次输入路径（供 TouchWakeByReset）
 */
void TouchInput_AfterWake(void);
