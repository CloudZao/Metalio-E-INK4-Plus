#pragma once

#include <lvgl.h>

#include "book_screen/settings/book_tap_zones.h"

/** @brief 先展示加载页，再后台解析；避免点「开始阅读」后长时间无响应 */
void ShowOpenProgress(int percent);
/** @brief 启动打开书籍 worker */
void StartOpenWorker(int book_index);
/** @brief 异步进入正文（LVGL 任务入口） */
void StartReadAsync(void* user_data);
/** @brief 执行触摸分区动作 */
void DispatchTapZoneAction(BookTapZoneAction action);
/** @brief Tick 持锁后、timer_handler 前排空无锁投递的 OpenDone */
void BookDrainPendingOpenDone();
