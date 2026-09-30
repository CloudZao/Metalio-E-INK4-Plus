/**
 * @brief S31 RTC：PCF8563 + TCA P16（RTC_INT，低有效）
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 PCF8563（I2C1）；RTC_INT 走 TCA P16
 * @note 1.4：INT 低有效直进 P16（不再经 MOSFET→ESP GPIO）
 */
void MetalioRtc_Init(void);

/** @brief PCF8563 是否已就绪 */
bool MetalioRtc_Ok(void);

/**
 * @brief 是否有未消费的 RTC INT（P16 低→高边沿采样）
 */
bool MetalioRtc_IrqPending(void);

/**
 * @brief 消费边沿标志，并尝试清芯片 AF/TF
 * @return 调用前是否 pending
 */
bool MetalioRtc_ConsumeIrq(void);

/** @brief 1=P16 低（可能有未清 INT）；0=空闲 */
int MetalioRtc_GpioLevel(void);

#ifdef __cplusplus
}
#endif
