#pragma once

#include <lvgl.h>
#include "business/reader/reader_types.h"

/** @brief 将可见项属性及可选封面请求入队；slot 为空时只读取属性，由后台准备后提交 UI */
void EnqueueListCoverFill(const BookInfo& info, int max_w, int max_h, lv_obj_t* host, RasterImage* slot);

/** @brief 以单个 worker 处理当前页，保留最新待处理页且逐图等待 UI 回收 */
void ScheduleListCoverFill();

/** @brief 作废详情封面代次，取消在途解码并释放尚未执行的请求。 */
void CancelDetailCoverWorker();
