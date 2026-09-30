#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化震动马达（需 TCA 已就绪）；默认强度 1
 */
void MetalioVibe_Init(void);

/**
 * @brief 短震一次（连续模式或强度 0 时忽略）
 */
void MetalioVibe_Pulse(void);

/**
 * @brief 连续震动开关（老化等）
 * @param on true 持续开，false 关
 */
void MetalioVibe_Set(bool on);

/**
 * @brief 设置震动强度 0–3；0 不震，默认 1
 */
void MetalioVibe_SetIntensity(uint8_t level);

/**
 * @brief 当前强度档位
 */
uint8_t MetalioVibe_GetIntensity(void);

#ifdef __cplusplus
}
#endif
