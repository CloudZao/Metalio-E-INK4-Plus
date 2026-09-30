#pragma once

#include "screen_common.h"
#include "vk_key_handler.h"

/** @brief 从书库页请求打开百问 */
void RequestOpenAssistantFromBook();
/** @brief 从正文请求打开百问 */
void RequestOpenAssistantFromReader();
/** @brief 请求退回阅读应用首页 */
void RequestBackHome();
/** @brief 书库页 vk_home 长按进百问的键描述 */
VkKeyScreenDesc BookAiLongPressDesc(ScreenFactory factory);
/** @brief 盖板长按连翻一步 */
bool BookPageRepeatStep(int page_delta);
