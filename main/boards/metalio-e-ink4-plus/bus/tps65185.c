
#include "tps65185.h"
#include "metalio_i2c1.h"
#include "pca9555.h"
#include "epd_board.h"
#include "esp_err.h"
#include "esp_log.h"

#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

static const char* TAG_TPS = "tps65185";
static const int EPDIY_TPS_ADDR = 0x68;
#define I2C1_LOCK_MS 100

static esp_err_t i2c_master_read_slave_ex(i2c_port_t i2c_num, int reg, uint8_t* out) {
    uint8_t r_data[1] = {0};

    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EPDIY_TPS_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        metalio_i2c1_unlock();
        return ret;
    }

    cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EPDIY_TPS_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, r_data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    metalio_i2c1_unlock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (out) {
        *out = r_data[0];
    }
    return ESP_OK;
}

static uint8_t i2c_master_read_slave(i2c_port_t i2c_num, int reg) {
    uint8_t v = 0;
    esp_err_t ret = i2c_master_read_slave_ex(i2c_num, reg, &v);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_TPS, "read reg 0x%02X: %s", reg, esp_err_to_name(ret));
        return 0;
    }
    return v;
}

static esp_err_t i2c_master_write_slave(i2c_port_t i2c_num, uint8_t ctrl, uint8_t* data_wr, size_t size) {
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EPDIY_TPS_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, ctrl, true);

    i2c_master_write(cmd, data_wr, size, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    metalio_i2c1_unlock();
    return ret;
}

esp_err_t tps_write_register(i2c_port_t port, int reg, uint8_t value) {
    uint8_t w_data[1] = {value};
    return i2c_master_write_slave(port, reg, w_data, 1);
}

uint8_t tps_read_register(i2c_port_t i2c_num, int reg) {
    return i2c_master_read_slave(i2c_num, reg);
}

esp_err_t tps_read_register_ex(i2c_port_t i2c_num, int reg, uint8_t* value) {
    return i2c_master_read_slave_ex(i2c_num, reg, value);
}

esp_err_t tps_probe(i2c_port_t i2c_num) {
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EPDIY_TPS_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    metalio_i2c1_unlock();
    return ret;
}

void tps_set_vcom(i2c_port_t i2c_num, unsigned vcom_mV) {
    unsigned val = vcom_mV / 10;
    esp_err_t err = tps_write_register(i2c_num, 4, (val & 0x100) >> 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TPS, "VCOM2 write: %s", esp_err_to_name(err));
        return;
    }
    err = tps_write_register(i2c_num, 3, val & 0xFF);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TPS, "VCOM1 write: %s", esp_err_to_name(err));
    }
}

int8_t tps_read_thermistor(i2c_port_t i2c_num) {
    tps_write_register(i2c_num, TPS_REG_TMST1, 0x80);
    int tries = 0;
    while (true) {
        uint8_t val = tps_read_register(i2c_num, TPS_REG_TMST1);
        if (val & 0x20) {
            break;
        }
        tries++;
        if (tries >= 100) {
            ESP_LOGE("epdiy", "thermistor read timeout!");
            break;
        }
    }
    return (int8_t)tps_read_register(i2c_num, TPS_REG_TMST_VALUE);
}

void tps_vcom_kickback() {
    printf("VCOM Kickback test\n");
    epd_current_board()->measure_vcom(epd_ctrl_state());
    tps_write_register(I2C_NUM_0, 4, 0x38);
    vTaskDelay(1);
    tps_read_register(I2C_NUM_0, TPS_REG_INT1);
    tps_read_register(I2C_NUM_0, TPS_REG_VCOM2);
}

void tps_vcom_kickback_start() {
    tps_read_register(I2C_NUM_0, TPS_REG_INT1);
    tps_write_register(I2C_NUM_0, TPS_REG_VCOM2, 0xA0);
}

unsigned tps_vcom_kickback_rdy() {
    uint8_t int1reg = tps_read_register(I2C_NUM_0, TPS_REG_INT1);

    if (int1reg == 0x02) {
        uint8_t lsb = tps_read_register(I2C_NUM_0, 3);
        uint8_t msb = tps_read_register(I2C_NUM_0, 4);
        int u16Value = (lsb | (msb << 8)) & 0x1ff;
        ESP_LOGI("vcom", "raw value:%d temperature:%d C", u16Value, tps_read_thermistor(I2C_NUM_0));
        return u16Value * 10;
    }
    return 0;
}

void debug_tps_error(void) {
    uint8_t status = tps_read_register(I2C_NUM_0, TPS_REG_INT1);
    ESP_LOGE("TPS_HARDWARE", "TPS INT1 Reg(0x07): 0x%02X", status);

    if (status & (1 << 7)) ESP_LOGE("TPS", "VCOM 短路/过流故障");
    if (status & (1 << 6)) ESP_LOGE("TPS", "VNEG (-15V) 短路/过流故障");
    if (status & (1 << 5)) ESP_LOGE("TPS", "VPOS (+15V) 短路/过流故障");
    if (status & (1 << 4)) ESP_LOGE("TPS", "VGL (-20V) 短路/过流故障");
    if (status & (1 << 3)) ESP_LOGE("TPS", "VGH (+22V) 短路/过流故障");
}
