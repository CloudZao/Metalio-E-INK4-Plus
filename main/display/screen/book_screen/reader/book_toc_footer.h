#pragma once

#include <lvgl.h>

/** @brief 重绘阻塞期间 tick 仍走：复位长按计时，避免恢复后一次判定就 LONG_PRESSED */
void ResetPointerLongPress();
/** @brief 同步底栏时钟定时器 */
void SyncFooterClockTimer();
/** @brief 停止底栏时钟定时器 */
void StopFooterClockTimer();
/** @brief 刷新正文底栏 */
void UpdateReadFooter();
/** @brief 渲染目录列表 */
void RenderTocList();
