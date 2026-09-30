#pragma once

#include "epd_board.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Metalio E-Ink4-Plus 板定义（并行 ED047TC2） */
extern const EpdBoardDefinition epd_board_metalio_eink4_plus;

/**
 * @brief 最近一次 TPS65185 poweron 是否成功
 */
bool epd_board_metalio_eink4_plus_tps_ok(void);

/**
 * @brief TCA9555 是否已探测并配置成功
 */
bool epd_board_metalio_eink4_plus_ioexp_ok(void);

/**
 * @brief 脉冲 FT6336U RST（TCA P12），低 ≥5ms 后拉高并等待 300ms
 */
void epd_board_metalio_eink4_plus_tp_reset(void);

/**
 * @brief 震动马达输出（TCA P0.3），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 */
void epd_board_metalio_eink4_plus_motor_set(bool on);

/**
 * @brief 功放使能（TCA P0.4=PA_EN），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 */
void epd_board_metalio_eink4_plus_pa_set(bool on);

/**
 * @brief 模拟开关（ESP GPIO36=ANA_SW）；虚拟 U 盘时置低，关闭后置高
 * @param high true 置高，false 置低
 */
void epd_board_metalio_eink4_plus_ana_sw_set(bool high);

/**
 * @brief 入睡关轨：P5 摄像屏电 → P14 VOUT（最后关）
 */
void epd_board_metalio_eink4_plus_sleep_cut_rails(void);

/**
 * @brief 浅睡醒后恢复 P14 → P5
 */
void epd_board_metalio_eink4_plus_sleep_restore_rails(void);

/**
 * @brief 读 TCA 扩展脚输入电平（0..15；高口=io>>3，位=io&7）
 * @param io 扩展脚号（如 EPD_IO_VOL_UP）
 * @return true 高电平；TCA 未就绪或读失败时保持上次成功值（默认高）
 */
bool epd_board_metalio_eink4_plus_ioexp_get_level(int io);

/**
 * @brief 读 TCA 一整口输入（失败返回 false，*out 不变）
 */
bool epd_board_metalio_eink4_plus_ioexp_read_port(int port, uint8_t* out);

/**
 * @brief 等待 TCA INT（GPIO2 下降沿）；读输入口可清中断
 * @param timeout_ms 超时毫秒，0 表示不等待
 * @return true 等到中断
 */
bool epd_board_metalio_eink4_plus_ioexp_irq_wait(uint32_t timeout_ms);

/**
 * @brief 订阅 GPIO2 下降沿（与 TCA/SC7A20H INT 共用）；ISR 内 TaskNotifyGive
 * @param task 接收通知的任务；传 NULL 取消订阅
 */
void epd_board_metalio_eink4_plus_gpio2_irq_subscribe(TaskHandle_t task);

/**
 * @brief 按手册将 TPS65185 置入 SLEEP：PWRUP/VCOM↓ → WAKEUP↓ → 等待 ≥100ms
 * @note 深睡前调用；SLEEP 后 I2C 无效、寄存器复位
 */
void epd_board_metalio_eink4_plus_tps_enter_sleep(void);

/**
 * @brief 入睡前：释放 LCD 外设，屏幕并行通信脚全部输出低
 * @note D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER
 */
void epd_board_metalio_eink4_plus_bus_hold_low(void);

/**
 * @brief 启动/深睡醒后：解除屏总线 gpio_hold（深睡 latch 会跨复位残留）
 */
void epd_board_metalio_eink4_plus_boot_release_holds(void);

/**
 * @brief 入睡前：gpio_hold_en 保持总线脚低电平（深/浅睡）
 */
void epd_board_metalio_eink4_plus_bus_latch_for_sleep(void);

/**
 * @brief 浅睡醒后：解除 hold 并重新 epd_lcd_init
 */
void epd_board_metalio_eink4_plus_bus_restore(void);

/**
 * @brief 入睡前：Port0/Port1 全部改输出；TPS 控制脚置低；音量/电源脚驱空闲高；P17 随后改输入唤醒
 */
void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void);

/**
 * @brief 武装唤醒前：仅把电源键 P17 改回输入
 * @return true 配置成功
 */
bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void);

/**
 * @brief 浅睡醒后：恢复 Port0/Port1 默认输入输出（音量键等）
 */
void epd_board_metalio_eink4_plus_ioexp_restore_keys(void);

/**
 * @brief 向开关机芯片发关机脉冲：IO_P13 高/低各 PWR_PULSE_INTERVAL_MS，共 PWR_PULSE_COUNT 次
 */
void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void);

#ifdef __cplusplus
}
#endif
