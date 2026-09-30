#pragma once

#include <lvgl.h>

#include "reader/reader_types.h"

/** @brief 把页插图挂到槽位控件 */
void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool page_images);
/** @brief 启动页插图解码 worker */
void StartPageImageWorker();
