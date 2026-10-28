#pragma once
#include <lvgl.h>

/** @brief 创建带统一黑框、圆角和留白的二维码容器；side 为二维码内容边长 */
lv_obj_t* UiCreateQrFrame(lv_obj_t* parent, lv_coord_t side);
