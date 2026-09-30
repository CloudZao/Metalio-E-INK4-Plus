/**
 * @brief CX25601N 充电 IC（I2C）驱动接口
 */
#pragma once

#include "power_i2c.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CX25601N_I2C_ADDR           0x6B // 7-bit I2C 地址

#define CX25601N_ICHG_MIN_MA        80   // 充电电流下限 (mA)
#define CX25601N_ICHG_MAX_MA        3040 // 充电电流上限 (mA)
#define CX25601N_ICHG_STEP_MA       80   // 充电电流步进 (mA)

#define CX25601N_IINDPM_MIN_MA      100  // 输入限流下限 (mA)
#define CX25601N_IINDPM_MAX_MA      3000 // 输入限流上限 (mA)
#define CX25601N_IINDPM_STEP_MA     20   // 输入限流步进 (mA)

#define CX25601N_VREG_MIN_MV        3840 // 充电截止电压下限 (mV)
#define CX25601N_VREG_MAX_MV        4800 // 充电截止电压上限 (mV)
#define CX25601N_VREG_STEP_MV       10   // 充电截止电压步进 (mV)

#define CX25601N_VOTG_MIN_MV        3840 // OTG 输出电压下限 (mV)
#define CX25601N_VOTG_MAX_MV        5280 // OTG 输出电压上限 (mV)
#define CX25601N_VOTG_STEP_MV       80   // OTG 输出电压步进 (mV)

#define CX25601N_IOTG_MIN_MA        100  // OTG 输出电流下限 (mA)
#define CX25601N_IOTG_MAX_MA        1200 // OTG 输出电流上限 (mA)
#define CX25601N_IOTG_STEP_MA       20   // OTG 输出电流步进 (mA)

#define CX25601N_OTG_DEFAULT_MV     5040 // OTG 默认约 5.04 V
#define CX25601N_OTG_DEFAULT_MA     1000 // OTG 默认约 1 A

#define CX25601N_CHG_STAT_NOT       0 // 未充电/终止
#define CX25601N_CHG_STAT_CC        1 // 涓流/预充/CC
#define CX25601N_CHG_STAT_CV        2 // 恒压降流
#define CX25601N_CHG_STAT_TOPOFF    3 // Top-off

/**
 * @brief 初始化 CX25601N（含 VREG/EN_TERM/再充 kick 与欠压 BATFET 保护任务）
 * @param bus 旧版 I2C 总线描述（与电量计同总线）
 */
esp_err_t cx25601n_init(const power_i2c_t* bus);

/**
 * @brief 是否已初始化且可用
 */
bool cx25601n_is_ready(void);

/**
 * @brief 浅睡前挂起 VREG 监控任务
 */
void cx25601n_vreg_suspend_for_sleep(void);

/**
 * @brief 浅睡醒后恢复 VREG 监控任务
 */
void cx25601n_vreg_resume_after_sleep(void);

/**
 * @brief 使能/关闭充电
 */
esp_err_t cx25601n_enable_charge(bool enable);

/**
 * @brief 读取充电使能状态
 */
esp_err_t cx25601n_is_charge_enabled(bool *enabled);

/**
 * @brief 使能/关闭 OTG Boost（开时约 5V/1A，先关充电再置 EN_OTG）
 */
esp_err_t cx25601n_enable_otg(bool enable);

/**
 * @brief 读取 OTG 使能状态
 */
esp_err_t cx25601n_is_otg_enabled(bool *enabled);

/**
 * @brief 设置充电电流 (mA)
 */
esp_err_t cx25601n_set_ichg_ma(uint32_t ma);

/**
 * @brief 读取充电电流设定 (mA)
 */
esp_err_t cx25601n_get_ichg_ma(uint32_t *ma);

/**
 * @brief 设置输入限流 (mA)
 */
esp_err_t cx25601n_set_iindpm_ma(uint32_t ma);

/**
 * @brief 读取输入限流设定 (mA)
 */
esp_err_t cx25601n_get_iindpm_ma(uint32_t *ma);

/**
 * @brief 设置充电截止电压 (mV)
 */
esp_err_t cx25601n_set_vreg_mv(uint32_t mv);

/**
 * @brief 读取充电截止电压设定 (mV)
 */
esp_err_t cx25601n_get_vreg_mv(uint32_t *mv);

/**
 * @brief 读取充电状态码（REG0x1E CHG_STAT）
 */
esp_err_t cx25601n_get_chrg_stat(uint8_t *stat);

/**
 * @brief 读取 VBUS 状态码
 */
esp_err_t cx25601n_get_vbus_stat(uint8_t *stat);

/**
 * @brief 读寄存器
 */
esp_err_t cx25601n_read_reg(uint8_t reg, uint8_t *val);

/**
 * @brief 进入 BATFET Shutdown（切断电池→SYS，欠压保护）
 */
esp_err_t cx25601n_enter_batfet_shutdown(void);

/**
 * @brief 充电状态码转可读字符串
 */
const char *cx25601n_chrg_stat_str(uint8_t stat);

/**
 * @brief VBUS 状态码转可读字符串
 */
const char *cx25601n_vbus_stat_str(uint8_t stat);

#ifdef __cplusplus
}
#endif
