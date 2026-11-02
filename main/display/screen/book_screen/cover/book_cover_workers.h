#pragma once

#include <lvgl.h>

#include "reader/reader_types.h"

/** @brief 将封面读取及缩放请求入队，由后台准备后提交 UI */
void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot);
/** @brief 以单个 worker 处理当前页，保留最新待处理页且逐图等待 UI 回收 */
void ScheduleListCoverFill();
