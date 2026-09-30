#pragma once

#include "assistant_screen_priv.h"

/** @brief 总页超过上限时从 flow 头裁掉旧内容 */
bool Assistant_TrimFlowToMaxPages();
/** @brief 追加 flow 分片并视需要跳到新页 */
void Assistant_AppendFlowAndShow(FlowList&& chunk, bool jump_to_new);
/** @brief 从本地会话存储回放历史到 flow */
void Assistant_ReplayHistoryFromStore();
/** @brief 清空当前会话 flow/pages 与相关 UI */
void Assistant_ClearSession();
/** @brief 流式分片：排队或立即展示 */
void Assistant_QueueOrShowChunk(FlowList&& chunk, bool jump_to_new);
