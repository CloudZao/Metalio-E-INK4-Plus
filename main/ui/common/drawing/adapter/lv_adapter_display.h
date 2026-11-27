#pragma once

#include "sdkconfig.h"
#include "ui/common/runtime/input/screen_gesture.h"
#if CONFIG_DISPLAY_BACKEND_SSD1677
#include "ui/common/drawing/adapter/ssd1677/lv_adapter_ssd1677.h"
using LVAdapterDisplay = LvAdapterSsd1677;
#else
#include "ui/common/drawing/adapter/epdiy/lv_adapter_epdiy.h"
using LVAdapterDisplay = LvAdapterEpdiy;
#endif

#if !CONFIG_DISPLAY_BACKEND_SSD1677
#include <lvgl.h>

/** @brief 查询屏内触点是否按下 */
bool TouchUiFingerIsDown();

/** @brief 查询尚未交付的触摸边沿 */
bool TouchUiHasPendingEdges();
#endif
