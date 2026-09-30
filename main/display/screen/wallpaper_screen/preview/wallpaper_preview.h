#pragma once

#include <lvgl.h>

/** @brief 清除预览图 */
void ClearPreviewImage();
/** @brief 调度加载预览 */
void ScheduleLoadPreview(int index);
/** @brief 打开壁纸预览 */
void OpenPreview(int index);
/** @brief 关闭壁纸预览 */
void ClosePreview();
/** @brief 构建预览页主体 */
void BuildPreviewBody(lv_obj_t* scr, lv_coord_t status_h);
