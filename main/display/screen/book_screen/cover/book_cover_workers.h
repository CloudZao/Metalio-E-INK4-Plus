#pragma once

#include <string>

#include <lvgl.h>

#include "reader/reader_types.h"

/** @brief 将列表封面补全请求入队 */
void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot);
/** @brief 调度书架列表封面异步填充 */
void ScheduleListCoverFill();
/** @brief 路径是否标记为内嵌封面缺失 */
bool IsCoverEmbedMiss(const std::string& path);
