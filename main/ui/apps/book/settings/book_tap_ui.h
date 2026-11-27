#pragma once

#include <lvgl.h>
#include "business/reader/book_tap_zones.h"

/** @brief 触摸分区动作文案 */
const char* TapZoneActionLabel(BookTapZoneAction action);

/** @brief 在阅读设置正文区铺九宫格或动作选择，高度与首页卡片区一致 */
void BuildTapZoneSettings(lv_obj_t* body, int content_h);

/** @brief 当前是否在为某个分区选择动作 */
bool TapZonePickOpen();

/** @brief 复位分区默认 */
void OnTapZoneReset(lv_event_t* e);
