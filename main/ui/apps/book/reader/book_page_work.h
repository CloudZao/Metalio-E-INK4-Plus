#pragma once
#include "business/reader/reader_page_job.h"

/** @brief 提交一次页面准备任务，成功后由工作模块持有结果对象。 */
bool StartReaderPageWork(std::unique_ptr<ReaderPageResult> result);

/** @brief 在 UI 取走已完成的页面结果，不等待工作任务。 */
std::unique_ptr<ReaderPageResult> TakeCompletedReaderPageWork();

/** @brief 查询当前页面任务是否正在装载目标章节正文。 */
bool ReaderChapterLoadPending();
