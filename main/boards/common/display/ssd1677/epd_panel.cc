#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "epd_panel.h"
#include "config.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"

#define TAG "EpdPanel"

void EpdPanel_Init(esp_lcd_panel_io_handle_t* panel_io, esp_lcd_panel_handle_t* panel) {
    if (panel_io == nullptr || panel == nullptr) {
        return;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SSD1677_PANEL_BUFFER_SIZE + 8,
    };
    ESP_LOGD(TAG, "SSD1677: spi_bus_initialize...");
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGD(TAG, "SSD1677: spi_bus_initialize ok");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = EPD_PIN_CS,
        .dc_gpio_num = EPD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = EPD_SPI_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPD_SPI_HOST, &io_config, panel_io));

    esp_lcd_ssd1677_config_t ssd1677_cfg = {
        .busy_gpio_num = EPD_PIN_BUSY,
        .non_copy_mode = true,
        .use_fast_full_update = true,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = EPD_PIN_RST,
        .vendor_config = &ssd1677_cfg,
        .flags =
            {
                .reset_active_high = false,
            },
    };

    // BUSY 脚 ISR 需要 GPIO ISR service
    ESP_LOGD(TAG, "SSD1677: gpio_install_isr_service...");
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }
    ESP_LOGD(TAG, "SSD1677: gpio_install_isr_service ok (%s)", esp_err_to_name(isr_ret));

    ESP_LOGD(TAG, "SSD1677: new_panel...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1677(*panel_io, &panel_cfg, panel));
    ESP_LOGD(TAG, "SSD1677: new_panel ok");

    ESP_LOGD(TAG, "SSD1677: panel_reset...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGD(TAG, "SSD1677: panel_reset ok");

    ESP_LOGD(TAG, "SSD1677: panel_init...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel, true));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "SSD1677 ready %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}
