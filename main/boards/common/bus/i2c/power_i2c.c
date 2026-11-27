#include <esp_rom_sys.h>
#include "power_i2c.h"

static bool take_lock(const power_i2c_t* bus, int timeout_ms) {
    return bus->lock == NULL || bus->lock(timeout_ms);
}
static void give_lock(const power_i2c_t* bus) {
    if (bus->unlock != NULL) {
        bus->unlock();
    }
}
static int transfer_timeout(int ms) {
    return ms > 0 ? ms : 50;
}

esp_err_t power_i2c_probe(const power_i2c_t* bus, uint8_t addr, int timeout_ms) {
    if (bus == NULL || addr > 0x7f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_bus_handle_t handle;
    esp_err_t ret = i2c_master_get_bus_handle(bus->port, &handle);
    if (ret == ESP_OK) {
        ret = i2c_master_probe(handle, addr, transfer_timeout(timeout_ms));
    }
    give_lock(bus);
    return ret;
}

esp_err_t power_i2c_write_reg(const power_i2c_t* bus, uint8_t addr, uint8_t reg, const uint8_t* data, size_t len,
                              int timeout_ms) {
    if (bus == NULL || bus->device == NULL || addr > 0x7f || (len && data == NULL))
        return ESP_ERR_INVALID_ARG;
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_dev_handle_t device;
    esp_err_t ret = bus->device(bus->port, addr, &device);
    if (ret == ESP_OK) {
        i2c_master_transmit_multi_buffer_info_t buffers[] = {
            {.write_buffer = &reg, .buffer_size = 1},
            {.write_buffer = data, .buffer_size = len},
        };
        ret = i2c_master_multi_buffer_transmit(device, buffers, len ? 2 : 1, transfer_timeout(timeout_ms));
    }
    give_lock(bus);
    return ret;
}

esp_err_t power_i2c_write_read(const power_i2c_t* bus, uint8_t addr, uint8_t reg, uint8_t* data, size_t len,
                               int timeout_ms) {
    if (bus == NULL || bus->device == NULL || addr > 0x7f || data == NULL || len == 0)
        return ESP_ERR_INVALID_ARG;
    if (!take_lock(bus, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_dev_handle_t device;
    esp_err_t ret = bus->device(bus->port, addr, &device);
    const int ms = transfer_timeout(timeout_ms);
    if (ret == ESP_OK) {
        /* Preserve repeated START first, then the existing STOP-separated retry. */
        ret = i2c_master_transmit_receive(device, &reg, 1, data, len, ms);
        if (ret != ESP_OK) {
            esp_rom_delay_us(100);
            ret = i2c_master_transmit(device, &reg, 1, ms);
            if (ret == ESP_OK) {
                esp_rom_delay_us(100);
                ret = i2c_master_receive(device, data, len, ms);
            }
        }
    }
    give_lock(bus);
    return ret;
}
