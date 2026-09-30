#pragma once

#include "home_screen_priv.h"

/** @brief 填充斜切布局应用页 */
void Home_FillSlashAppsPage();
/** @brief 创建斜切布局单个应用格 */
void Home_CreateSlashAppCell(lv_obj_t* row, const AppEntry& entry, int row_i, int col, lv_coord_t row_h,
                             lv_coord_t cell_w);
/** @brief 斜切单元格自定义绘制 */
void Home_OnSlashCellDraw(lv_event_t* e);
