#include "power_i2c.h"

#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>

static bool take_lock(const power_i2c_t* bus, int timeout_ms) {
    if (bus == NULL || bus->lock == NULL) {
        return true;
    }
    return bus->lock(timeout_ms);
}

static void give_lock(const power_i2c_t* bus) {
    if (bus != NULL && bus->unlock != NULL) {
        bus->unlock();
    }
}

esp_err_t power_i2c_probe(const power_i2c_t* bus, uint8_t addr_7bit, int timeout_ms) {
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        give_lock(bus);
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(bus->port, cmd, pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 50));
    i2c_cmd_link_delete(cmd);
    give_lock(bus);
    return ret;
}

esp_err_t power_i2c_write_reg(const power_i2c_t* bus, uint8_t addr_7bit, uint8_t reg,
                              const uint8_t* data, size_t len, int timeout_ms) {
    if (bus == NULL || (len > 0 && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        give_lock(bus);
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (len > 0) {
        i2c_master_write(cmd, (uint8_t*)data, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(bus->port, cmd, pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 50));
    i2c_cmd_link_delete(cmd);
    give_lock(bus);
    return ret;
}

esp_err_t power_i2c_write_read(const power_i2c_t* bus, uint8_t addr_7bit, uint8_t reg,
                               uint8_t* data, size_t len, int timeout_ms) {
    if (bus == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    const int ticks = pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 50);

    // TI 标准：写命令 + repeated START + 读（与 397 i2c_master_transmit_receive 一致）
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        give_lock(bus);
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(bus->port, cmd, ticks);
    i2c_cmd_link_delete(cmd);

    // 失败再试 STOP 分隔 + tBUF（手册 ≥66µs@>100kHz；此处保守）
    if (ret != ESP_OK) {
        esp_rom_delay_us(100);
        cmd = i2c_cmd_link_create();
        if (cmd == NULL) {
            give_lock(bus);
            return ESP_ERR_NO_MEM;
        }
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(bus->port, cmd, ticks);
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            esp_rom_delay_us(100);
            cmd = i2c_cmd_link_create();
            if (cmd == NULL) {
                give_lock(bus);
                return ESP_ERR_NO_MEM;
            }
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_READ, true);
            if (len > 1) {
                i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
            }
            i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
            i2c_master_stop(cmd);
            ret = i2c_master_cmd_begin(bus->port, cmd, ticks);
            i2c_cmd_link_delete(cmd);
        }
    }

    give_lock(bus);
    return ret;
}
