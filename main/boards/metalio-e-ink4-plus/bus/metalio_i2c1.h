#pragma once

#include <driver/i2c.h>
#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2C1（TCA/TPS/触摸）总线锁；timeout_ms=0 立即返回
 * @return true 拿到锁
 */
bool metalio_i2c1_lock(int timeout_ms);

/** @brief 释放 I2C1 总线锁 */
void metalio_i2c1_unlock(void);

#ifdef __cplusplus
}
#endif
