/**
 * @brief SC7A20H 采样与阅读姿态缓存
 */
#pragma once

#include <stdbool.h>
#include "boards/common/bus/i2c/power_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 探测并初始化 SC7A20H
 * @param bus 板级总线配置，初始化时复制
 * @param subscribe_irq 可选共享中断订阅函数，在采样任务上下文调用
 */
void Accelerometer_Init(const power_i2c_t* bus, void (*subscribe_irq)());

/**
 * @brief 入睡前松开共享 INT1（未 Init 时也会探测并关）
 * @note 任务随休眠挂起，不删
 */
void Accelerometer_ReleaseIntLine(void);

/** @brief 加速度计是否就绪 */
bool Accelerometer_Ok(void);

/** @brief 启停阅读姿态采样；快速模式用于晃动翻页，停止时清除缓存 */
void Accelerometer_SetReadingTracking(bool enabled, bool rapid);

/**
 * @brief 获取最近的阅读重力样本
 * @param x X 轴加速度输出，单位 mg
 * @param y Y 轴加速度输出，单位 mg
 * @param z 可选 Z 轴加速度输出，单位 mg
 * @return 采样已启用且样本未过期时返回 true，否则输出不可用于姿态判定
 */
bool Accelerometer_GetReadingGravity(int* x, int* y, int* z);

/** @brief 启停 ELF 应用采样订阅，与阅读订阅独立 */
void Accelerometer_SetAppTracking(bool enabled);

/** @brief 获取未过期的应用三轴加速度样本，单位 mg */
bool Accelerometer_GetAppGravity(int* x, int* y, int* z);

typedef enum {
    AccelerometerOrientationUnknown = -2,  // 无有效稳定轴向
    AccelerometerOrientationPortrait = 0,  // 竖屏
    AccelerometerOrientationLeft = 1,      // 左横屏
    AccelerometerOrientationRight = 2,     // 右横屏
} AccelerometerOrientation;

/** @brief 按当前板型既有安装轴向将重力样本转换为屏幕方向 */
AccelerometerOrientation Accelerometer_GetReadingOrientation(void);

#ifdef __cplusplus
}
#endif
