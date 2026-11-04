#pragma once

#include "epd_board.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <esp_err.h>

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
 * @brief 租用共享复位脚并确认供电和复位已释放，不改写扩展器输出
 * @note 成功后须在同一任务调用 release，期间阻止触摸复位。
 */
esp_err_t epd_board_metalio_eink4_plus_camera_acquire(uint32_t timeout_ms);
/** @brief 释放当前任务持有的共享复位脚租约。 */
void epd_board_metalio_eink4_plus_camera_release(void);

/**
 * @brief 震动马达输出（TCA P0.3），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 */
void epd_board_metalio_eink4_plus_motor_set(bool on);

/**
 * @brief 功放使能（TCA P0.4=PA_EN），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 * @return 成功返回 ESP_OK；复位期间开启或写入失败返回对应错误
 */
esp_err_t epd_board_metalio_eink4_plus_pa_set(bool on);
/** @brief P14 共享电源拉低后执行马达放电，再恢复供电；调用方须阻止待机。 */
esp_err_t epd_board_metalio_eink4_plus_bt_power_reset(void);

/**
 * @brief 模拟开关（ESP GPIO36=ANA_SW）；虚拟 U 盘时置低，关闭后置高
 * @param high true 置高，false 置低
 */
void epd_board_metalio_eink4_plus_ana_sw_set(bool high);

/**
 * @brief 入睡关轨：P5 摄像屏电 → P14 VOUT（最后关）
 * @return 成功返回 ESP_OK；失败时可能已部分关轨，调用方须恢复
 */
esp_err_t epd_board_metalio_eink4_plus_sleep_cut_rails(void);

/**
 * @brief 退出待机或绘制关机画面前分阶段恢复 P14 → P5
 * @return 成功返回 ESP_OK，失败返回对应错误
 */
esp_err_t epd_board_metalio_eink4_plus_sleep_restore_rails(void);

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
 * @return 成功返回 ESP_OK，失败时不继续推进后续休眠步骤
 */
esp_err_t epd_board_metalio_eink4_plus_tps_enter_sleep(void);

/**
 * @brief 入睡前：释放 LCD 外设，屏幕并行通信脚全部输出低
 * @note D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER
 */
void epd_board_metalio_eink4_plus_bus_hold_low(void);

/**
 * @brief 启动时清理一次握手 DMA 与 GPIO 中断残留，并解除屏总线保持状态
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
 * @return 成功返回 ESP_OK，失败返回对应错误；失败后须恢复，不能继续配置唤醒
 */
esp_err_t epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void);

/**
 * @brief 配置电源键输入，清除方向切换中断并确认电源键已松开
 * @return true 配置及采样成功且电源键已松开
 */
bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void);

/**
 * @brief 浅睡醒后：恢复 Port0/Port1 默认输入输出（音量键等）
 * @return 配置及读口成功返回 ESP_OK，失败返回对应错误
 */
esp_err_t epd_board_metalio_eink4_plus_ioexp_restore_keys(void);

/**
 * @brief 向开关机芯片发关机脉冲：IO_P13 高/低各 PWR_PULSE_INTERVAL_MS，共 PWR_PULSE_COUNT 次
 */
void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void);

#ifdef __cplusplus
}
#endif
