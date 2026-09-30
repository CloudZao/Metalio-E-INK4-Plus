#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 连翻步进回调：page_delta 为 ±N 页；返回 false 表示触边应停止
 */
typedef bool (*VkPageRepeatStepFn)(int page_delta);

/** @brief 停止当前连翻状态 */
void VkPageRepeatStop(void);

/** @brief 若 key 是 vk_prev / vk_next 则启动连翻；触边自动停止 */
bool VkPageRepeatTryStart(const char* key, VkPageRepeatStepFn step);

/** @brief 松手时停止连翻；返回 true 表示已消费 */
bool VkPageRepeatOnPressUp(const char* key);

/** @brief 当前是否处于连翻中 */
bool VkPageRepeatIsActive(void);

#ifdef __cplusplus
}
#endif
