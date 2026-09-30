/**
 * @brief S31 电源外设：BQ27220 电量计 + CX25601N 充电管理/欠压保护
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在 EPD 板级 I2C1 就绪后初始化电量计与充电 IC
 * @note 芯片未焊接时跳过，不阻塞启动
 */
void MetalioPower_Init(void);

#ifdef __cplusplus
}
#endif
