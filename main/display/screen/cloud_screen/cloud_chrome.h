#pragma once

#include <lvgl.h>

/** @brief 调度用户触发的云同步 */
void ScheduleUserCloudSync();
/** @brief 云页 Tab 点击 */
void OnTabClicked(lv_event_t* e);
/** @brief 创建 Tab 按钮 */
lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab);
/** @brief 同步按钮点击 */
void OnSyncClicked(lv_event_t* e);
/** @brief 构建云页主 chrome */
void BuildCloudMainChrome(lv_obj_t* scr, lv_coord_t status_height);
/** @brief 构建壁纸预览 chrome */
void BuildWallpaperPreviewChrome(lv_obj_t* scr, lv_coord_t status_height);
