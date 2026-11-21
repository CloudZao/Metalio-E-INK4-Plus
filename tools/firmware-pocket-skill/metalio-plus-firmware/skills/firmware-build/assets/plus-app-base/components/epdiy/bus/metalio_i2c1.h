#pragma once

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One shared synchronous I2C1 bus. All *_locked calls require this mutex. */
bool metalio_i2c1_lock(int timeout_ms);
bool metalio_i2c1_lock_for_flash(int timeout_ms);
void metalio_i2c1_unlock(void);
esp_err_t metalio_i2c1_init(gpio_num_t sda, gpio_num_t scl);
esp_err_t metalio_i2c1_deinit(void);
esp_err_t metalio_i2c1_device_locked(i2c_port_t port, uint8_t addr,
                                     i2c_master_dev_handle_t* device);
esp_err_t metalio_i2c1_probe_locked(i2c_port_t port, uint8_t addr, int timeout_ms);
esp_err_t metalio_i2c1_transmit_locked(i2c_port_t port, uint8_t addr,
                                       const uint8_t* data, size_t len, int timeout_ms);
esp_err_t metalio_i2c1_receive_locked(i2c_port_t port, uint8_t addr,
                                      uint8_t* data, size_t len, int timeout_ms);
esp_err_t metalio_i2c1_transmit_receive_locked(i2c_port_t port, uint8_t addr,
    const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_len, int timeout_ms);
esp_err_t metalio_i2c1_write_reg_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    const uint8_t* data, size_t len, int timeout_ms);
/* STOP between pointer write and receive, as required by existing TPS/TCA calls. */
esp_err_t metalio_i2c1_read_reg_stop_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    uint8_t* data, size_t len, int timeout_ms);

#ifdef __cplusplus
}
#endif

