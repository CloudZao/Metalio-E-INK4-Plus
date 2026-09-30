/**
 * @brief 板级电源芯片共用的旧版 I2C 读写（带可选总线锁）
 */
#pragma once

#include <driver/i2c.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t port;                      // I2C 端口
    bool (*lock)(int timeout_ms);         // 可选总线锁；NULL 表示不加锁
    void (*unlock)(void);                 // 与 lock 成对
} power_i2c_t;

/**
 * @brief 写地址探测（START + 写地址 + STOP）
 */
esp_err_t power_i2c_probe(const power_i2c_t* bus, uint8_t addr_7bit, int timeout_ms);

/**
 * @brief 写寄存器：先发 reg，再发 data[len]
 */
esp_err_t power_i2c_write_reg(const power_i2c_t* bus, uint8_t addr_7bit, uint8_t reg,
                              const uint8_t* data, size_t len, int timeout_ms);

/**
 * @brief 写寄存器地址后读 len 字节
 */
esp_err_t power_i2c_write_read(const power_i2c_t* bus, uint8_t addr_7bit, uint8_t reg,
                               uint8_t* data, size_t len, int timeout_ms);

#ifdef __cplusplus
}
#endif
