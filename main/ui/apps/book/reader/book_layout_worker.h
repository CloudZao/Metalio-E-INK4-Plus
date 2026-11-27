#pragma once

#include <lvgl.h>

/** @brief 启动当前阅读章节重排任务 */
void StartLayoutWorker();

/** @brief 在 UI 任务接收当前章节的排版终态。 */
void PollReaderLayoutWork();

/** @brief 偏好变更后重开正文会话 */
void ReopenReaderAfterPrefsChange();

/** @brief 离开阅读时取消等待中的偏好重开 */
void CancelReaderPrefsReopen();

/** @brief 目录行点击 */
void OnTocRowClicked(lv_event_t* e);

/** @brief 正文内容区点击（九宫格） */
void OnReadContentClicked(lv_event_t* e);
