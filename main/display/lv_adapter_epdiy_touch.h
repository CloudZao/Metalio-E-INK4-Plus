#pragma once

#include <lvgl.h>

/** @brief 注册 touch_feed 采样回调（盖板键 / 屏内早震） */
void LvAdapterEpdiy_TouchBind(void);

/** @brief LVGL indev 读点（锁存，无 I2C） */
void LvAdapterEpdiy_TouchReadCb(lv_indev_t* indev, lv_indev_data_t* data);
