#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "touch_input.h"
#include "config.h"
#include "boards/common/bus/i2c/i2c_device_adapters.h"
#include "boards/common/display/ssd1677/esp_lcd_ssd1677_commands.h"

static const char* TAG = "TouchInput";

// TCA9555 P1.1 = io_index 9（TP RST；脚位权威见 config.h / 原理图）
static const uint32_t kTouchRstMask = (1U << 9);

static esp_err_t touch_rst_set(esp_io_expander_handle_t expander, int level) {
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_INVALID_ARG, TAG, "expander null");
    return esp_io_expander_set_level(expander, kTouchRstMask, level ? 1 : 0);
}

esp_err_t touch_input_hw_reset_power_on(esp_io_expander_handle_t expander) {
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_INVALID_ARG, TAG, "expander null");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, kTouchRstMask, IO_EXPANDER_OUTPUT), TAG, "RST dir");
    ESP_RETURN_ON_ERROR(touch_rst_set(expander, 0), TAG, "RST L");
    vTaskDelay(pdMS_TO_TICKS(10));  // Tpr ≥5ms
    ESP_RETURN_ON_ERROR(touch_rst_set(expander, 1), TAG, "RST H");
    vTaskDelay(pdMS_TO_TICKS(120));  // Tron ≥100ms
    return ESP_OK;
}

esp_err_t touch_input_hw_reset_pulse(esp_io_expander_handle_t expander) {
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_INVALID_ARG, TAG, "expander null");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, kTouchRstMask, IO_EXPANDER_OUTPUT), TAG, "RST dir");

    ESP_RETURN_ON_ERROR(touch_rst_set(expander, 1), TAG, "RST H");
    vTaskDelay(pdMS_TO_TICKS(2 + portTICK_PERIOD_MS - 1));
    ESP_RETURN_ON_ERROR(touch_rst_set(expander, 0), TAG, "RST L");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(touch_rst_set(expander, 1), TAG, "RST H");
    vTaskDelay(pdMS_TO_TICKS(120));  // Tron
    ESP_LOGI(TAG, "TP_RST pulse done (P1.1/P11 H->L->H)");
    return ESP_OK;
}

esp_err_t touch_input_init(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t* out_touch) {
    ESP_RETURN_ON_FALSE(i2c_bus != NULL && out_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    *out_touch = NULL;

    esp_err_t probe = I2cDeviceAdapters_Probe(ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, 200);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "CST816S not found on I2C (0x%02X): %s", ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
                 esp_err_to_name(probe));
        return probe;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_err_t ret = I2cDeviceAdapters_NewTouch(ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, &tp_io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch panel_io create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // CST816S 原生竖屏 480x800；勿 swap_xy（与 LVGL ROTATE_270 逻辑分辨率一致）
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = SSD1677_PANEL_HEIGHT,
        .y_max = SSD1677_PANEL_WIDTH,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };

    esp_lcd_touch_handle_t touch = NULL;
    ret = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &touch);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CST816S init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(tp_io);
        return ret;
    }

    esp_err_t pull = gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY);
    if (pull != ESP_OK) {
        ESP_LOGW(TAG, "touch INT pull-up failed: %s", esp_err_to_name(pull));
    }

    ESP_LOGI(TAG, "CST816S ready (SDA=%d SCL=%d INT=%d) native 480x800 IRQ", I2C_SDA_PIN, I2C_SCL_PIN,
             (int)TOUCH_INT_GPIO);
    *out_touch = touch;
    return ESP_OK;
}

esp_err_t touch_input_enter_sleep(esp_lcd_touch_handle_t touch) {
    if (touch == NULL || touch->io == NULL) {
        ESP_LOGW(TAG, "enter_sleep: no touch");
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t sleep_cmd = 0x03;
    esp_err_t err = esp_lcd_panel_io_tx_param(touch->io, 0xA5, &sleep_cmd, 1);
    ESP_LOGD(TAG, "touch sleep A5=03: %s", esp_err_to_name(err));
    return err;
}

esp_err_t touch_input_wake_by_reset(esp_io_expander_handle_t expander, touch_input_after_wake_cb_t after_wake_cb) {
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_INVALID_ARG, TAG, "expander null");

    esp_err_t err = touch_input_hw_reset_pulse(expander);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch wake RST failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGD(TAG, "touch wake RST done (P1.1/P11, no reg write)");
    if (after_wake_cb != NULL) {
        after_wake_cb();
    }
    return ESP_OK;
}
