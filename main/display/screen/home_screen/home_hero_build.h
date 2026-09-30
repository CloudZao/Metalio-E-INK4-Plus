#pragma once

#include "home_hero_priv.h"

/** @brief 斜切 Hero 间隙自定义绘制 */
void HomeHero_OnSlashGapDraw(lv_event_t* e);
/** @brief 斜切时间标签样式 */
void HomeHero_StyleSlashTimeLabel(lv_obj_t* lbl);
/** @brief 构建经典 Hero UI */
void HomeHero_BuildClassicUi(lv_obj_t* parent, lv_coord_t y_offset);
/** @brief 构建斜切 Hero UI */
void HomeHero_BuildSlashUi(lv_obj_t* parent, lv_coord_t y_offset);
