#pragma once

#include "assistant_screen_priv.h"

/** @brief 百问屏生命周期事件（加载/卸载） */
void Assistant_OnAssistantLifecycle(lv_event_t* e);
/** @brief 取百问屏 UI 共享状态 */
AssistantUiState& Assistant_State();
/** @brief BOOT 双击：拍照插入对话 */
bool Assistant_OnBootDoubleClick();
/** @brief 异步打开百问页 */
void Assistant_OpenAssistantAsync(void* arg);
/** @brief 盖板键长按连翻 */
bool Assistant_OnVkKeyLongPress(const char* key);
/** @brief BOOT 抬起：结束 PTT */
bool Assistant_OnBootPressUp();
/** @brief BOOT 按下：开始 PTT */
bool Assistant_OnBootPressDown();
/** @brief BOOT 短按单击钩子 */
bool Assistant_OnBootClick();
/** @brief 盖板键抬起：停止连翻 */
bool Assistant_OnVkKeyPressUp(const char* key);
/** @brief BOOT 长按钩子 */
bool Assistant_OnBootLongPress();
/** @brief 盖板键短按翻页 */
bool Assistant_OnVkKey(const char* key);
