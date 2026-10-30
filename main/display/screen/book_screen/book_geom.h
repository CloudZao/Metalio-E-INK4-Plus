#pragma once

#include <lvgl.h>

#include "reader/reader.h"

inline constexpr lv_coord_t kReaderVerticalSideInset = 20; // 竖排正文在列起始侧增加的留白，单位像素

/** @brief 按正文是否可见同步阅读 AA；设置卡片覆盖时正文仍可见 */
void SyncReaderAaForImmersion(bool text_visible);
/** @brief 把 NVS 排版偏好绑定到 BookSession */
void BindSessionLayoutPrefs(reader::BookSession& session);
/** @brief 计算四边距内的正文可绘区域；方格模式只使用居中的完整网格范围 */
lv_area_t ReadContentArea();
/** @brief 打开进度 UI 用书库边距；避免阅读页边距档位导致 Book_ContentWidth 溢出 */
void ApplyOverlayListContentPads(lv_coord_t top_pad);
/** @brief 应用阅读方向并更新正文、阅读菜单和排版；须在 LVGL 任务调用 */
void ApplyReaderOrient(int orient);
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
