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

/** @brief 启停阅读姿态采样，复用加速度计任务；停止时清除缓存 */
void MetalioAccel_SetReadingTracking(bool enabled);

/**
 * @brief 获取最近的阅读重力样本
 * @param x X 轴加速度输出，单位 mg
 * @param y Y 轴加速度输出，单位 mg
 * @return 采样已启用且样本未过期时返回 true，否则输出不可用于姿态判定
 */
bool MetalioAccel_GetReadingGravity(int* x, int* y);

#ifdef __cplusplus
}
#endif
