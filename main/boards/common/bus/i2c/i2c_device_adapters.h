#pragma once
#include <esp_io_expander.h>
#include <esp_lcd_panel_io.h>
#include <driver/i2c_master.h>
#ifdef __cplusplus
extern "C" {
#endif

/** @brief 通过470共享总线接入397触摸传输接口 */
esp_err_t I2cDeviceAdapters_NewTouch(uint8_t addr, esp_lcd_panel_io_handle_t* out);

/** @brief 通过470共享总线接入397扩展器驱动接口 */
esp_err_t I2cDeviceAdapters_NewExpander(uint8_t addr, esp_io_expander_handle_t* out);

/** @brief 通过共享总线探测397外设 */
esp_err_t I2cDeviceAdapters_Probe(uint8_t addr, int timeout_ms);
#ifdef __cplusplus
}
#endif
