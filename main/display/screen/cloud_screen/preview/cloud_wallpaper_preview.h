#pragma once

#include <lvgl.h>

/** @brief 下载按钮样式 */
void StyleDownloadBtn(bool enabled);
/** @brief 清除壁纸预览图 */
void ClearWallpaperPreviewImage();
/** @brief 进入壁纸预览模式 */
void ShowWallpaperPreviewMode(bool preview);
/** @brief 关闭壁纸预览 */
void CloseWallpaperPreview();
/** @brief 调度加载壁纸预览图 */
void ScheduleLoadWallpaperPreview(int index);
/** @brief 打开壁纸预览 */
void OpenWallpaperPreview(int index);
/** @brief 开始壁纸下载 */
void StartWallpaperDownload(int index);
/** @brief 壁纸下载按钮点击 */
void OnWallpaperDownloadClicked(lv_event_t* e);
