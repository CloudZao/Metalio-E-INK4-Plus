#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** @brief 读取震动开关，首次调用加载持久偏好 */
bool HapticIsEnabled(void);

/** @brief 更新震动开关并合并异步保存；写入失败保留 RAM 状态，后续请求可重试 */
void HapticSetEnabled(bool enabled);

/** @brief 读取震动档位，0 弱、1 中、2 强，默认弱 */
int HapticGetIntensity(void);

/** @brief 更新震动档位并合并异步保存，无效值恢复弱档；失败保留 RAM 状态 */
void HapticSetIntensity(int intensity);

/** @brief 按当前偏好请求一次反馈，无马达时为空操作 */
void HapticPulseIfEnabled(void);

/** @brief 按偏好开关与档位请求限时脉冲；返回脉冲标识，未启动返回零 */
uint32_t HapticPulseForIfEnabled(int duration_ms);
#ifdef __cplusplus
}
#endif
