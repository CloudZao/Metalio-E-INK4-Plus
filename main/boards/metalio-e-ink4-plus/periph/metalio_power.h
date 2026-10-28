/**
 * @brief S31 电源外设：BQ27220 电量计 + CX25601N 充电管理/欠压保护
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在 EPD 板级 I2C1 就绪后初始化电量计与充电 IC
 * @note 芯片未焊接时跳过，不阻塞启动
 */
void MetalioPower_Init(void);

/** @brief 获取充电电流档位，单位 mA；默认 1000 */
int MetalioPower_GetChargeCurrentMa(void);
/** @brief 设置并保存 500、1000 或 1500mA 充电档位，失败返回 false */
bool MetalioPower_SetChargeCurrentMa(int ma);

#ifdef __cplusplus
}
#endif
