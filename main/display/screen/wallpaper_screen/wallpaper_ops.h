#pragma once

#include <string>

#include <lvgl.h>

/** @brief 调度设为关机/待机壁纸 */
void ScheduleEnable(int index, bool for_standby, bool clear);
/** @brief 设为关机壁纸 */
void OnEnableShutdownClicked(lv_event_t* e);
/** @brief 设为待机壁纸 */
void OnEnableStandbyClicked(lv_event_t* e);
/** @brief 删除 SD 上壁纸文件 */
bool DeleteWallpaperFileOnSd(const char* path, const char* name, std::string& err_out);
/** @brief 调度删除壁纸 */
void ScheduleDelete(int index);
/** @brief 删除按钮点击 */
void Wallpaper_OnDeleteClicked(lv_event_t* e);
