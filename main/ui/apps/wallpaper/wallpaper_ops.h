#pragma once

#include <lvgl.h>

/** @brief 设为关机壁纸 */
void OnEnableShutdownClicked(lv_event_t* e);

/** @brief 设为待机壁纸 */
void OnEnableStandbyClicked(lv_event_t* e);

/** @brief 设置或取消当前图片的阅读背景。 */
void OnEnableReadingClicked(lv_event_t* e);

/** @brief 调度删除壁纸 */
void ScheduleDelete(int index);

/** @brief 删除按钮点击 */
void Wallpaper_OnDeleteClicked(lv_event_t* e);
