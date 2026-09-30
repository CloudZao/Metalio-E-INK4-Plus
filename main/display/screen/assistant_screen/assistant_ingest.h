#pragma once

#include "assistant_screen_priv.h"

/** @brief 解析 A2UI 消息为 FlowItem 列表 */
IngestResult Assistant_IngestA2uiMessage(cJSON* msg, FlowList& out);
/** @brief 将一轮对话 JSON 持久化到会话存储 */
void Assistant_PersistTurnJson(cJSON* msg);
