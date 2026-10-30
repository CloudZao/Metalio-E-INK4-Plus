#pragma once

#include "reader/book_session.h"

struct ReaderPageResult {
    std::unique_ptr<reader::BookSession> cursor; // 独立导航与页面所有权
    reader::RasterImage background; // 仅背景几何变化时生成
    reader::RasterImage raster; // 后台完成的正文像素
    lv_coord_t column_width = 0; // 正文实际列宽
    int ink_height = 0; // 正文墨迹高度
    int value = 0; // 请求参数
    int direction = 0; // 实际源位置的移动方向，与正文像素一同发布
    reader::BookSession::NavigationKind kind = reader::BookSession::NavigationKind::kRefresh; // 请求类型
    uint32_t token = 0; // 会话代次
    int64_t navigation_us = 0, raster_us = 0, queue_us = 0; // 后台各阶段耗时
    int64_t started_us = 0; // 请求被接受时刻
    bool ok = false; // 导航和物化结果
};

/** @brief 提交一次页面准备任务，成功后由工作模块持有结果对象。 */
bool StartReaderPageWork(std::unique_ptr<ReaderPageResult> result);
/** @brief 在 UI 取走已完成的页面结果，不等待工作任务。 */
std::unique_ptr<ReaderPageResult> TakeCompletedReaderPageWork();
