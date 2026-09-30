#pragma once

#include <lvgl.h>

#include "reader/reader.h"

/** @brief 设置卡片盖在正文上：关掉即可，不必重绘；目录整页需恢复正文 */
void SyncReaderAaForImmersion(bool immersive);
/** @brief 把 NVS 排版偏好绑定到 BookSession */
void BindSessionLayoutPrefs(reader::BookSession& session);
/** @brief 正文内容区宽度（阅读页边距）；与沉浸态居中栏宽一致。 书库/详情继续用 Book_ContentWidth()（kListPad），互不影响。 */
lv_coord_t ReadContentWidth();
/** @brief 打开进度 UI 用书库边距；避免阅读页边距档位导致 Book_ContentWidth 溢出 */
void ApplyOverlayListContentPads(lv_coord_t top_pad);
/** @brief 是否整页图模式（未 Open 时用 Peek，便于建屏设 viewport） */
bool ReaderPageImagesHint();
/** @brief 勿把 *_busy 置 false，否则其它路径会误 ReleaseBookFont 与计页抢字体 */
void StopReadTimeCheckpointTimer();
/** @brief 正文可读后开始累计；须在 LVGL 任务调用 */
void BeginReadingTimeTracking();
/** @brief 换新 session 前停旧 timer/墙钟，避免回调 UAF 或漏写本段秒数 */
void EndReadingTimeTracking();
/** @brief 进待机：停累计；退出待机由 ResumeReadingTimeAfterStandby 恢复 */
void PauseReadingTimeForStandby();
/** @brief 退出待机后恢复阅读时长累计 */
void ResumeReadingTimeAfterStandby();

// IsReadOverlayChrome / ApplyReaderPageGeometry：依赖 priv 中的枚举，声明见 book_screen_priv.h
