#pragma once

#include "screen_common_priv.h"

/** @brief 清空返回栈 */
void ScreenCommon_ClearBackStack();
/** @brief 压入返回工厂 */
void ScreenCommon_PushBackFactory(ScreenFactory factory);
/** @brief 弹出返回工厂（空则返回 nullptr） */
ScreenFactory ScreenCommon_PopBackFactory();
/** @brief 异步回首页（lv_async 入口） */
void ScreenCommon_GoHomeAsync(void* user_data);
/** @brief 异步返回上一屏（lv_async 入口） */
void ScreenCommon_NavigateBackAsync(void* user_data);
