/**
 * @brief 充电电流管理
 */
#pragma once
#include <stdbool.h>
#include "config.h"
#include "boards/common/bus/i2c/power_i2c.h"

inline constexpr int kChargeCurrentOptionsMa[] = BOARD_CHARGE_CURRENT_OPTIONS_MA;  // 板型支持的充电电流档位，单位 mA

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用板级 I2C 总线初始化充电 IC
 * @param bus 板级总线配置
 * @param settings_namespace 持久化命名空间，须保持至关机
 * @param settings_key 电流配置键，须保持至关机
 * @note 芯片未焊接时跳过，不阻塞启动
 */
void ChargerControl_Init(const power_i2c_t* bus, const char* settings_namespace, const char* settings_key);

/** @brief 获取充电电流档位，单位 mA；初始值由板型配置确定 */
int ChargerControl_GetChargeCurrentMa(void);

/**
 * @brief 设置板型支持的充电档位并请求后台保存
 * @return 硬件设置及保存请求成功返回 true；失败返回 false
 * @note 调度或写入失败保留当前档位及待保存状态，后续同档位请求可重试。
 */
bool ChargerControl_SetChargeCurrentMa(int ma);

#ifdef __cplusplus
}
#endif
