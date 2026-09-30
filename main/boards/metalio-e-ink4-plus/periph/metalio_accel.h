/**
 * @brief S31 加速度计 SC7A20H（I2C1；INT1 与 TCA 共用 GPIO2）
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 探测并初始化 SC7A20H（先 0x19 再 0x18）
 * @note 运行：INT1 开漏低有效 + AOI1；入睡前 ReleaseIntLine 改为推挽拉高松开线与
 */
void MetalioAccel_Init(void);

/**
 * @brief 入睡前松开共享 INT1（未 Init 时也会探测并关）
 * @note 任务随休眠挂起，不删
 */
void MetalioAccel_ReleaseIntLine(void);

/** @brief 加速度计是否就绪 */
bool MetalioAccel_Ok(void);

#ifdef __cplusplus
}
#endif
