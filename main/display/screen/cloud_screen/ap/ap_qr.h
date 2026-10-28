#pragma once

#include <lvgl.h>

constexpr int kApQrSide = 240; // 二维码边长（含静区）

/** @brief 把文本画成二维码；画布需已设为 I1 缓冲 */
void ApQrPaint(lv_obj_t* canvas, const char* text);

/** @brief 创建带统一边框、圆角与留白的传输二维码画布 */
lv_obj_t* ApQrCreate(lv_obj_t* parent);
