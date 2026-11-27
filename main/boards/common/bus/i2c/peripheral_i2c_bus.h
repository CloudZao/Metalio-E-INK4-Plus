#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include "power_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// 共享同步 I2C 总线；带 Locked/locked 后缀的接口须由调用方持有总线锁。
/** @brief 等待并取得总线互斥锁，超时返回 false */
bool PeripheralI2cBus_Lock(int timeout_ms);

/** @brief 释放当前任务持有的总线互斥锁 */
void PeripheralI2cBus_Unlock(void);

/** @brief 按指定引脚初始化共享 I2C 主机 */
esp_err_t PeripheralI2cBus_Init(gpio_num_t sda, gpio_num_t scl);

/** @brief 关闭共享 I2C 主机并释放设备句柄 */
esp_err_t PeripheralI2cBus_Deinit(void);

/** @brief 取得指定地址的缓存设备句柄，不向调用方转移其所有权 */
esp_err_t PeripheralI2cBus_DeviceLocked(i2c_port_t port, uint8_t addr, i2c_master_dev_handle_t* device);

/** @brief 在已锁定的总线上探测设备地址 */
esp_err_t PeripheralI2cBus_ProbeLocked(i2c_port_t port, uint8_t addr, int timeout_ms);

/** @brief 在已锁定的总线上发送数据 */
esp_err_t PeripheralI2cBus_TransmitLocked(i2c_port_t port, uint8_t addr, const uint8_t* data, size_t len,
                                          int timeout_ms);

/** @brief 在已锁定的总线上接收数据 */
esp_err_t PeripheralI2cBus_ReceiveLocked(i2c_port_t port, uint8_t addr, uint8_t* data, size_t len, int timeout_ms);

/** @brief 在已锁定的总线上执行连续写读事务 */
esp_err_t PeripheralI2cBus_TransmitReceiveLocked(i2c_port_t port, uint8_t addr, const uint8_t* tx, size_t tx_len,
                                                 uint8_t* rx, size_t rx_len, int timeout_ms);

/** @brief 在已锁定的总线上发送寄存器地址及数据 */
esp_err_t PeripheralI2cBus_WriteRegLocked(i2c_port_t port, uint8_t addr, uint8_t reg, const uint8_t* data, size_t len,
                                          int timeout_ms);
// 此读接口在寄存器地址写入与数据接收之间发送 STOP。
/** @brief 在已锁定的总线上分两次事务读取寄存器 */
esp_err_t PeripheralI2cBus_ReadRegStopLocked(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t* data, size_t len,
                                             int timeout_ms);

/** @brief 获取共享外设总线描述 */
const power_i2c_t* PeripheralI2cBus_PowerBus(void);

/** @brief 创建或重建共享外设总线 */
void PeripheralI2cBus_Create(i2c_master_bus_handle_t* bus);

#ifdef __cplusplus
}
#endif
