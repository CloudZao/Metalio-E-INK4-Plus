#pragma once

#include "screen_common_priv.h"

/** @brief 顶栏网络图标点击：未联网 WiFi 可进扫网页 */
void ScreenCommon_OnStatusNetworkIconClicked(lv_event_t* e);
/** @brief 取 LVAdapterDisplay 单例 */
LVAdapterDisplay* ScreenCommon_GetAdapterDisplay();
/** @brief 弹出返回工厂 */
ScreenFactory ScreenCommon_PopBackFactory();
/** @brief 异步回首页 */
void ScreenCommon_GoHomeAsync(void* user_data);
/** @brief 清空返回栈 */
void ScreenCommon_ClearBackStack();
/** @brief 异步返回上一屏 */
void ScreenCommon_NavigateBackAsync(void* user_data);
/** @brief 压入返回工厂 */
void ScreenCommon_PushBackFactory(ScreenFactory factory);
