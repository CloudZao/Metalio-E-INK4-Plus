#pragma once

#include <lvgl.h>

/** @brief 启动全文/章节重排 worker */
void StartLayoutWorker();
/** @brief 启动章节内翻页 worker */
void StartChapterPageWorker();
/** @brief 偏好变更后重开正文会话 */
void ReopenReaderAfterPrefsChange();
/** @brief 目录行点击 */
void OnTocRowClicked(lv_event_t* e);
/** @brief 正文内容区点击（九宫格） */
void OnReadContentClicked(lv_event_t* e);
