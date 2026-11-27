#pragma once

#include <lvgl.h>
#include "business/reader/book_tap_zones.h"

/** @brief 先展示加载页，再后台解析；避免点「开始阅读」后长时间无响应 */
void ShowOpenProgress(int percent);

/** @brief 启动打开书籍 worker */
void StartOpenWorker(int book_index);

/** @brief 异步进入正文（LVGL 任务入口） */
void StartReadAsync(void* user_data);

/** @brief 执行触摸分区动作 */
void DispatchTapZoneAction(BookTapZoneAction action);

/** @brief 恢复阅读时交付已暂停的打开结果，失效结果先完成收尾。 */
void BookApplyDeferredOpenResult();
