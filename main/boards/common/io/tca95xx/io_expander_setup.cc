#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "io_expander_setup.h"
#include "config.h"
#include "boards/common/bus/i2c/peripheral_i2c_bus.h"
#include "touch_input.h"
#include "boards/common/io/IOExpander.hpp"
#include "boards/common/power/power_hw.h"

#define TAG "IoExpanderSetup"

bool IoExpanderSetup_Init(i2c_master_bus_handle_t* bus) {
    if (bus == nullptr) {
        return false;
    }
    auto& io = IOExpander::getInstance();
    esp_err_t err = io.begin(*bus, IO_EXPANDER_I2C_ADDR);
    if (err != ESP_OK && io.handle() == nullptr) {
        ESP_LOGW(TAG, "TCA9555 begin failed (%s), recreate I2C bus and retry", esp_err_to_name(err));
        PeripheralI2cBus_Create(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        err = io.begin(*bus, IO_EXPANDER_I2C_ADDR);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9555 unavailable (%s); continue without IO expander", esp_err_to_name(err));
        return false;
    }
    // 先开总电源，再开屏幕卡座供电（begin 后输出锁存默认高，此处显式钉住）
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::MAIN_PWR, true));
    // 对齐 power_hw 影子态（仅 IO 上电时 known=false，is_on 会误报 0）
    power_hw_main_rail_set(true);
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::SCREEN_SOCKET_PWR, true));
    // 功放路由给 ESP32；PA 由 PowerPolicy 按 Speaking/通话 按需开，不在开机强开
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PA_SWITCH, false));
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PA, false));
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true));
    // USB 默认走烧录/调试通路，拍照时再切到摄像头（见 UsbUvcStill）
    ESP_ERROR_CHECK(io.setLevel(IOExpander::Pin::USB_MUX_SEL, true));
    // 触摸 RST 上电复位；关机脉冲空闲拉高
    ESP_ERROR_CHECK(touch_input_hw_reset_power_on(io.handle()));
    ESP_LOGI(TAG,
             "MAIN_PWR + SCREEN_SOCKET_PWR on; PA off (policy); USB_MUX=flash; "
             "TP_RST L->H; PWR_KEY=H");

    gpio_config_t int_conf = {};
    int_conf.pin_bit_mask = 1ULL << IO_EXPANDER_INT_GPIO;
    int_conf.mode = GPIO_MODE_INPUT;
    int_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_conf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t int_err = gpio_config(&int_conf);
    if (int_err != ESP_OK) {
        ESP_LOGW(TAG, "IO expander INT GPIO%d config failed: %s", static_cast<int>(IO_EXPANDER_INT_GPIO),
                 esp_err_to_name(int_err));
    } else {
        ESP_LOGI(TAG, "IO expander INT on GPIO%d; ACCEL_INT on TCA9555 P1.4", static_cast<int>(IO_EXPANDER_INT_GPIO));
    }
    return true;
}
