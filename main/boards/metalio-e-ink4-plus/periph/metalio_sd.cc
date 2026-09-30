/**
 * @file metalio_sd.cc
 * @brief S31 TF：挂载走 SdCardManager；休眠脚位保持仍在本文件
 */

#include "metalio_sd.h"

#include "SdCardManager.hpp"
#include "config.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <esp_rom_gpio.h>
#include <sdkconfig.h>
#include <soc/gpio_sig_map.h>

#if CONFIG_IDF_TARGET_ESP32S31
#include <soc/cnnt_io_mux_struct.h>
#endif

#define TAG "MetalioSd"

static const gpio_num_t kSdPins[] = {
    SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN, SDMMC_D1_PIN, SDMMC_D2_PIN, SDMMC_D3_PIN,
};

#if CONFIG_IDF_TARGET_ESP32S31
static void sdio_cnnt_release_to_gpio(void) {
    CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpd = 1;
    CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpd = 1;
    CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpd = 1;
    CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpd = 1;
    CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpd = 1;
    CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpu = 0;
    CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpd = 1;
    CNNT_PAD_CTRL.ctrl.sdio_pad_pin_ctrl_ded_sel = 0;
    ESP_LOGI(TAG, "CNNT SDIO pad released to GPIO (ded_sel=0)");
}
#endif

static void sd_pin_force_low(gpio_num_t pin) {
    gpio_hold_dis(pin);
    esp_gpio_revoke(1ULL << static_cast<unsigned>(pin));
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << static_cast<unsigned>(pin),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
    gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    gpio_set_level(pin, 0);
}

void MetalioSd_ReleaseHold(void) {
    for (gpio_num_t pin : kSdPins) {
        gpio_hold_dis(pin);
    }
}

bool MetalioSd_Init(void) {
    MetalioSd_ReleaseHold();
    return SdCardManager::GetInstance().Mount();
}

bool MetalioSd_Ok(void) {
    return SdCardManager::GetInstance().IsMounted();
}

void MetalioSd_HoldPinsLow(void) {
    MetalioSd_ReleaseHold();
    SdCardManager::GetInstance().Unmount();
#if CONFIG_IDF_TARGET_ESP32S31
    sdio_cnnt_release_to_gpio();
#endif
    for (gpio_num_t pin : kSdPins) {
        sd_pin_force_low(pin);
    }
    ESP_LOGI(TAG, "SD pins held low");
}

void MetalioSd_LatchForSleep(void) {
    for (gpio_num_t pin : kSdPins) {
        gpio_set_level(pin, 0);
        esp_err_t err = gpio_hold_en(pin);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_hold_en(%d): %s", (int)pin, esp_err_to_name(err));
        }
    }
}
