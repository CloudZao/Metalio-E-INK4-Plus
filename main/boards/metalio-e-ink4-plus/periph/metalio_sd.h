#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载 SD 卡（SDMMC Slot0 4-bit）；可重复调用
 * @return true 挂载成功
 */
bool MetalioSd_Init(void);

/**
 * @brief SD 是否已挂载（检测正常）
 */
bool MetalioSd_Ok(void);

/**
 * @brief 卸载 SD 并将 CLK/CMD/D0..D3 推挽置低
 */
void MetalioSd_HoldPinsLow(void);

/**
 * @brief 入睡前锁存 SD 脚低电平（须先 HoldPinsLow）
 */
void MetalioSd_LatchForSleep(void);

/**
 * @brief 解除 SD 脚 hold（浅睡醒后 remount 前 / 启动时）
 */
void MetalioSd_ReleaseHold(void);

#ifdef __cplusplus
}
#endif
