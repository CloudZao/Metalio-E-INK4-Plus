#pragma once

#include "assistant_screen_priv.h"

/** @brief 仅重分页尾部（流式跟刷） */
int Assistant_RebuildPagesTail();
/** @brief 将当前页索引钳到合法范围 */
void Assistant_ClampPageIndex();
/** @brief 按内容区几何全量重分页 */
void Assistant_RebuildPages();
