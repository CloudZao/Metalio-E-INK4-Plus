#pragma once

/** @brief 获取阅读页实际方向，自动模式使用最近确认的方向 */
int ReaderDisplayOrient();

/** @brief 按当前偏好启停自动方向判定；须在 LVGL 任务调用 */
void SyncReaderAutoOrient();

/** @brief 停止自动方向判定并释放采样；须在 LVGL 任务调用 */
void StopReaderAutoOrient();
