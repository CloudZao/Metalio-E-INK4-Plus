#pragma once

#include <lvgl.h>

#include "reader/reader_types.h"

/**
 * @brief 将阅读位图挂到槽位控件
 * @param center 正文插图传 true；文字栅格和阅读背景传 false，保留其灰度策略
 */
void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool center);
/** @brief 启动页插图解码 worker */
void StartPageImageWorker();
