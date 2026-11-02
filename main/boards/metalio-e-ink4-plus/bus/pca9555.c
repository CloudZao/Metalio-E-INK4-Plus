

#include "pca9555.h"
#include "metalio_i2c1.h"

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#define I2C1_LOCK_MS 100

#define REG_INPUT_PORT0 0
#define REG_INPUT_PORT1 1

#define REG_OUTPUT_PORT0 2
#define REG_OUTPUT_PORT1 3

#define REG_INVERT_PORT0 4
#define REG_INVERT_PORT1 5

#define REG_CONFIG_PORT0 6
#define REG_CONFIG_PORT1 7

static uint8_t s_pca9555_addr = EPDIY_PCA9555_ADDR;
static uint32_t s_next_error_log_ms;

static void log_i2c_error(const char* op, uint8_t reg, const char* stage,
                          int64_t start_us, int64_t locked_us, int64_t end_us, esp_err_t err) {
    const uint32_t now_ms = (uint32_t)(end_us / 1000);
    uint32_t next_ms = __atomic_load_n(&s_next_error_log_ms, __ATOMIC_RELAXED);
    if ((next_ms && (int32_t)(now_ms - next_ms) < 0) ||
        !__atomic_compare_exchange_n(&s_next_error_log_ms, &next_ms, now_ms + 1000,
                                     false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return;
    }
    ESP_LOGW("PCA9555", "%s reg=0x%02x stage=%s wait_us=%lld xfer_us=%lld task=%s core=%d prio=%u: %s",
             op, reg, stage, (long long)((locked_us ? locked_us : end_us) - start_us),
             (long long)(locked_us ? end_us - locked_us : 0), pcTaskGetName(NULL),
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL), esp_err_to_name(err));
}

void pca9555_set_addr(uint8_t addr_7bit) {
    s_pca9555_addr = addr_7bit & 0x7F;
}

uint8_t pca9555_get_addr(void) {
    return s_pca9555_addr;
}

static esp_err_t i2c_master_read_slave(i2c_port_t port, uint8_t* data, size_t size, int reg) {
    if (size == 0) return ESP_OK;
    const int64_t start_us = esp_timer_get_time();
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        log_i2c_error("read", reg, "lock", start_us, 0, esp_timer_get_time(), ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    const int64_t locked_us = esp_timer_get_time();
    const esp_err_t ret = metalio_i2c1_read_reg_stop_locked(port, s_pca9555_addr,
                                                          (uint8_t)reg, data, size, 50);
    const int64_t end_us = esp_timer_get_time();
    metalio_i2c1_unlock();
    if (ret != ESP_OK) log_i2c_error("read", reg, "transfer", start_us, locked_us, end_us, ret);
    return ret;
}

static esp_err_t i2c_master_write_slave(i2c_port_t port, uint8_t reg, uint8_t* data, size_t size) {
    const int64_t start_us = esp_timer_get_time();
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        log_i2c_error("write", reg, "lock", start_us, 0, esp_timer_get_time(), ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    const int64_t locked_us = esp_timer_get_time();
    const esp_err_t ret = metalio_i2c1_write_reg_locked(port, s_pca9555_addr, reg, data, size, 50);
    const int64_t end_us = esp_timer_get_time();
    metalio_i2c1_unlock();
    if (ret != ESP_OK) log_i2c_error("write", reg, "transfer", start_us, locked_us, end_us, ret);
    return ret;
}

static esp_err_t pca9555_write_single(i2c_port_t port, int reg, uint8_t value) {
    uint8_t w_data[1] = { value };
    return i2c_master_write_slave(port, reg, w_data, sizeof(w_data));
}

esp_err_t pca9555_set_config(i2c_port_t port, uint8_t config_value, int high_port) {
    return pca9555_write_single(port, REG_CONFIG_PORT0 + high_port, config_value);
}

esp_err_t pca9555_set_inversion(i2c_port_t port, uint8_t config_value, int high_port) {
    return pca9555_write_single(port, REG_INVERT_PORT0 + high_port, config_value);
}

esp_err_t pca9555_set_value(i2c_port_t port, uint8_t config_value, int high_port) {
    return pca9555_write_single(port, REG_OUTPUT_PORT0 + high_port, config_value);
}

uint8_t pca9555_read_input(i2c_port_t i2c_port, int high_port) {
    uint8_t v = 0;
    if (pca9555_read_input_ex(i2c_port, high_port, &v) != ESP_OK) {
        return 0;
    }
    return v;
}

esp_err_t pca9555_read_input_ex(i2c_port_t i2c_port, int high_port, uint8_t* out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_read_slave(i2c_port, out, 1, REG_INPUT_PORT0 + high_port);
    if (err != ESP_OK) {
        ESP_LOGD("PCA9555", "%s failed: %s", __func__, esp_err_to_name(err));
    }
    return err;
}
