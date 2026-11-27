#include <stdlib.h>
#include <esp_lcd_panel_io_interface.h>
#include <esp_check.h>
#include "i2c_device_adapters.h"
#include "peripheral_i2c_bus.h"

static const char* TAG = "I2cDeviceAdapters";

typedef struct {
    esp_lcd_panel_io_t base;  // 保留CST816S托管驱动的传输接口
    uint8_t addr;             // 设备地址，句柄由共享总线持有
} touch_io_t;

typedef struct {
    esp_io_expander_t base;  // 保留397现有引脚与电源控制接口
    uint8_t addr;            // 设备地址，句柄由共享总线持有
    uint16_t output;         // 与原TCA9555驱动一致的输出锁存缓存
    uint16_t direction;      // 与原TCA9555驱动一致的方向缓存
} expander_io_t;

esp_err_t I2cDeviceAdapters_Probe(uint8_t addr, int timeout_ms) {
    return power_i2c_probe(PeripheralI2cBus_PowerBus(), addr, timeout_ms);
}

static esp_err_t touch_read(esp_lcd_panel_io_t* io, int cmd, void* data, size_t len) {
    if (cmd < 0 || cmd > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    return power_i2c_write_read(PeripheralI2cBus_PowerBus(), ((touch_io_t*)io)->addr, (uint8_t)cmd, data, len, 50);
}

static esp_err_t touch_write(esp_lcd_panel_io_t* io, int cmd, const void* data, size_t len) {
    if (cmd < 0 || cmd > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    return power_i2c_write_reg(PeripheralI2cBus_PowerBus(), ((touch_io_t*)io)->addr, (uint8_t)cmd, data, len, 50);
}

static esp_err_t touch_delete(esp_lcd_panel_io_t* io) {
    free(io);
    return ESP_OK;
}

esp_err_t I2cDeviceAdapters_NewTouch(uint8_t addr, esp_lcd_panel_io_handle_t* out) {
    ESP_RETURN_ON_FALSE(out && addr <= 0x7f, ESP_ERR_INVALID_ARG, TAG, "invalid touch arguments");
    *out = NULL;
    touch_io_t* io = calloc(1, sizeof(*io));
    ESP_RETURN_ON_FALSE(io, ESP_ERR_NO_MEM, TAG, "touch transport allocation failed");
    io->addr = addr;
    io->base.rx_param = touch_read;
    io->base.tx_param = touch_write;
    io->base.del = touch_delete;
    *out = &io->base;
    return ESP_OK;
}

static esp_err_t expander_read(esp_io_expander_handle_t handle, uint32_t* value) {
    expander_io_t* io = (expander_io_t*)handle;
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(power_i2c_write_read(PeripheralI2cBus_PowerBus(), io->addr, 0, data, sizeof(data), 1000), TAG,
                        "read input failed");
    *value = data[0] | ((uint32_t)data[1] << 8);
    return ESP_OK;
}

static esp_err_t expander_write(esp_io_expander_handle_t handle, uint32_t value, bool direction) {
    expander_io_t* io = (expander_io_t*)handle;
    const uint8_t data[] = {(uint8_t)value, (uint8_t)(value >> 8)};
    ESP_RETURN_ON_ERROR(
        power_i2c_write_reg(PeripheralI2cBus_PowerBus(), io->addr, direction ? 6 : 2, data, sizeof(data), 1000), TAG,
        "write expander register failed");
    if (direction) {
        io->direction = (uint16_t)value;
    } else
        io->output = (uint16_t)value;
    return ESP_OK;
}

static esp_err_t expander_output(esp_io_expander_handle_t h, uint32_t v) {
    return expander_write(h, v, false);
}
static esp_err_t expander_direction(esp_io_expander_handle_t h, uint32_t v) {
    return expander_write(h, v, true);
}
static esp_err_t expander_get_output(esp_io_expander_handle_t h, uint32_t* v) {
    *v = ((expander_io_t*)h)->output;
    return ESP_OK;
}
static esp_err_t expander_get_direction(esp_io_expander_handle_t h, uint32_t* v) {
    *v = ((expander_io_t*)h)->direction;
    return ESP_OK;
}
static esp_err_t expander_delete(esp_io_expander_handle_t h) {
    free(h);
    return ESP_OK;
}
static esp_err_t expander_reset(esp_io_expander_handle_t h) {
    ESP_RETURN_ON_ERROR(expander_direction(h, 0xffff), TAG, "reset direction failed");
    return expander_output(h, 0xffff);
}

esp_err_t I2cDeviceAdapters_NewExpander(uint8_t addr, esp_io_expander_handle_t* out) {
    ESP_RETURN_ON_FALSE(out && addr <= 0x7f, ESP_ERR_INVALID_ARG, TAG, "invalid expander arguments");
    *out = NULL;
    expander_io_t* io = calloc(1, sizeof(*io));
    ESP_RETURN_ON_FALSE(io, ESP_ERR_NO_MEM, TAG, "expander transport allocation failed");
    io->addr = addr;
    io->base.config.io_count = 16;
    io->base.config.flags.dir_out_bit_zero = 1;
    io->base.read_input_reg = expander_read;
    io->base.write_output_reg = expander_output;
    io->base.read_output_reg = expander_get_output;
    io->base.write_direction_reg = expander_direction;
    io->base.read_direction_reg = expander_get_direction;
    io->base.del = expander_delete;
    io->base.reset = expander_reset;
    const esp_err_t err = expander_reset(&io->base);
    if (err != ESP_OK) {
        free(io);
        return err;
    }
    *out = &io->base;
    return ESP_OK;
}
