#pragma once

#include "ui/apps/assistant/assistant_screen_priv.h"

/** @brief 仅重分页尾部（流式跟刷） */
int Assistant_RebuildPagesTail();

/** @brief 增量重排语音回答尾部，缓存不适用时退回全量分页；返回需重绘的起始页。 */
int Assistant_RebuildYunxinTextTail();

/** @brief 清除语音回答的增量分页缓存。 */
void Assistant_ResetYunxinPagination();

/** @brief 将当前页索引钳到合法范围 */
void Assistant_ClampPageIndex();

/** @brief 按内容区几何全量重分页 */
void Assistant_RebuildPages();
