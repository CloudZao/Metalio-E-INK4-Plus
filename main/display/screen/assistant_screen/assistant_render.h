#pragma once

#include "assistant_screen_priv.h"

/** @brief 盖板长按连翻一步 */
bool Assistant_PageRepeatStep(int page_delta);
/** @brief 请求异步重绘当前页 */
void Assistant_RequestRenderCurrentPage();
/** @brief 刷新页码指示 */
void Assistant_UpdatePageIndicator();
/** @brief 立即渲染当前页到 a2ui */
void Assistant_RenderCurrentPage();
