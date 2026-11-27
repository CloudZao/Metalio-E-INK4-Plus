#pragma once

#include <driver/i2c_master.h>

/**
 * @brief 初始化 TCA9555：上电脚、触摸 RST、INT GPIO；失败可重建 I2C 后重试
 * @param bus I2C 总线指针（重试时可能被 PeripheralI2cBus_Create 重建）
 * @return true 扩展器可用
 */
bool IoExpanderSetup_Init(i2c_master_bus_handle_t* bus);
