/**
 * @file epd_board_common.c
 * @brief Temperature acquisition module for the EPD board.
 *
 * This file implements ADC-based ambient temperature reading using ESP-IDF's
 * One-shot ADC driver and optional calibration. The temperature information is
 * typically used by E-Paper waveform selection and compensation logic.
 *
 * Implementation overview:
 *  - ADC1 Channel 7 is used for temperature measurement.
 *  - ADC One-shot driver ensures low overhead single-read operations.
 *  - 100 samples are averaged to reduce ADC noise.
 *  - If ADC calibration is supported, curve-fitting calibration is used
 *    (ESP-IDF v5.3+), otherwise fallback to manual conversion.
 *  - Assumes TMP36-compatible analog temperature sensor:
 *
 *        Voltage(mV) = ADC_raw -> calibrated voltage
 *        Temperature(°C) = (Voltage_mV - 500) / 10
 *
 * Dependencies:
 *  - esp_adc/adc_oneshot.h
 *  - esp_adc/adc_cali.h
 *  - esp_adc/adc_cali_scheme.h
 *
 * Author: (Your Name / Project Team)
 * Date: (Optional)
 */


#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "epd_temperature";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;

#define NUMBER_OF_SAMPLES 100

void epd_board_temperature_init_v2()
{
    // 配置 ADC1 unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&init_config, &adc_handle);

    // 配置通道
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_7, &config);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "ADC Calibration OK");
    else
        ESP_LOGW(TAG, "ADC Calibration NOT supported");
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "ADC Calibration OK");
    else
        ESP_LOGW(TAG, "ADC Calibration NOT supported");
#else
    ESP_LOGW(TAG, "ADC Calibration NOT supported");
#endif
}

float epd_board_ambient_temperature_v2()
{
    int raw = 0;
    int value = 0;

    for (int i = 0; i < NUMBER_OF_SAMPLES; i++) {
        adc_oneshot_read(adc_handle, ADC_CHANNEL_7, &raw);
        value += raw;
    }

    value /= NUMBER_OF_SAMPLES;

    int voltage_mv = 0;

    if (adc_cali_handle) {
        adc_cali_raw_to_voltage(adc_cali_handle, value, &voltage_mv);
    } else {
        // 无校准则手工转换
        voltage_mv = value * 1100 / 4095;
    }

    // TMP36 格式 普通温度传感器
    return (voltage_mv - 500) / 10.0f;
}
