/*
 * CST816S 触摸适配：初始化 / 休眠 / 硬件 RST 唤醒
 * TP RST 引脚见板型 config（TCA9555 P1.1 / 原理图 P11）
 */
#pragma once

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_io_expander.h>
#include <esp_lcd_touch.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 上电复位序列：RST 低 → Tpr → 高 → Tron
 * @param expander TCA9555 句柄
 * @return ESP_OK 成功
 */
esp_err_t touch_input_hw_reset_power_on(esp_io_expander_handle_t expander);

/**
 * @brief 运行中复位脉冲：H → L → H（规格 Trst/Tpr/Tron）
 * @param expander TCA9555 句柄
 * @return ESP_OK 成功
 */
esp_err_t touch_input_hw_reset_pulse(esp_io_expander_handle_t expander);

/**
 * @brief 探测并创建 CST816S
 * @param i2c_bus I2C 主总线
 * @param out_touch 成功时非空；失败时为 NULL
 * @return ESP_OK 成功；失败返回错误码（不崩溃）
 */
esp_err_t touch_input_init(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t* out_touch);

/**
 * @brief 写 0xA5=0x03 进入触摸深睡
 * @param touch 触摸句柄
 * @return ESP_OK 成功
 */
esp_err_t touch_input_enter_sleep(esp_lcd_touch_handle_t touch);

/**
 * @brief 硬件 RST 唤醒后可选回调（例如通知 LVGL 触摸 indev）
 */
typedef void (*touch_input_after_wake_cb_t)(void);

/**
 * @brief A5=03 深睡后仅硬件 RST 脉冲唤醒（不写触摸寄存器）
 * @param expander TCA9555 句柄
 * @param after_wake_cb 唤醒后回调，可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t touch_input_wake_by_reset(esp_io_expander_handle_t expander, touch_input_after_wake_cb_t after_wake_cb);

#ifdef __cplusplus
}
#endif
