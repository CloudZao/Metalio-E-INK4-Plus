#include "metalio_i2c1.h"

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/i2c_ll.h>

static StaticSemaphore_t s_mu_storage;
static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_flash_time_mux = portMUX_INITIALIZER_UNLOCKED;
static TickType_t s_flash_ticks;
static TickType_t s_flash_started;
static bool s_flash_active;
static i2c_master_bus_handle_t s_bus;
static gpio_num_t s_sda, s_scl;
#define DEVICE_SLOTS 16
static struct { uint8_t addr; i2c_master_dev_handle_t handle; } s_devices[DEVICE_SLOTS];

static void ensure_mu(void) {
    if (__atomic_load_n(&s_mu, __ATOMIC_ACQUIRE) != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_mu == NULL) {
        /* Static creation: no heap allocation inside the interrupt-disabled lock. */
        __atomic_store_n(&s_mu, xSemaphoreCreateMutexStatic(&s_mu_storage), __ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&s_init_mux);
}

typedef struct {
    TickType_t wall; // 与 RTOS 等待使用同一时基
    TickType_t flash; // 累计受控占用，含尚未结束的窗口
} i2c_wait_time_t;

static i2c_wait_time_t wait_time_snapshot(void) {
    portENTER_CRITICAL(&s_flash_time_mux);
    const TickType_t now = xTaskGetTickCount();
    const i2c_wait_time_t result = {
        .wall = now,
        .flash = s_flash_ticks + (s_flash_active ? now - s_flash_started : 0),
    };
    portEXIT_CRITICAL(&s_flash_time_mux);
    return result;
}

bool metalio_i2c1_lock(int timeout_ms) {
    ensure_mu();
    if (s_mu == NULL) return false;
    if (xSemaphoreTake(s_mu, 0) == pdTRUE) return true;
    if (timeout_ms == 0) return false;
    if (timeout_ms < 0) return xSemaphoreTake(s_mu, portMAX_DELAY) == pdTRUE;

    // 先扩宽再换算，正 int 毫秒不能因 TickType_t 的乘法溢出而缩短预算。
    uint64_t remaining = (uint64_t)timeout_ms * configTICK_RATE_HZ / 1000;
    if (remaining == 0) return false;
    i2c_wait_time_t previous = wait_time_snapshot();
    while (remaining != 0) {
        const TickType_t chunk = remaining >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)remaining;
        // 队列在边界成功时保持原成功语义，不在取锁后追溯判为超时。
        if (xSemaphoreTake(s_mu, chunk) == pdTRUE) return true;
        const i2c_wait_time_t current = wait_time_snapshot();
        const TickType_t elapsed = current.wall - previous.wall;
        const TickType_t excluded = current.flash - previous.flash;
        // 两个时刻来自同一短临界区快照；分段差值支持 tick 与累计值回绕。
        const TickType_t charged = elapsed - excluded;
        if (charged >= remaining || (excluded == 0 && remaining <= chunk)) return false;
        remaining -= charged;
        previous = current;
    }
    return false;
}

static void quiet_idle_interrupts(void) {
    if (s_bus == NULL) return; // Do not touch a gated/deleted peripheral.
    /* All transfers are synchronous and share s_mu. The new driver rearms its
       event mask at each transfer/probe; keep the existing flash drain barrier. */
    i2c_ll_disable_intr_mask(I2C_LL_GET_HW(I2C_NUM_1), I2C_LL_INTR_MASK);
    i2c_ll_clear_intr_mask(I2C_LL_GET_HW(I2C_NUM_1), I2C_LL_INTR_MASK);
}

bool metalio_i2c1_lock_for_flash(int timeout_ms) {
    ensure_mu();
    const TaskHandle_t owner = s_mu != NULL ? xSemaphoreGetMutexHolder(s_mu) : NULL;
    if (owner != NULL && owner == xTaskGetCurrentTaskHandle()) return false;
    if (!metalio_i2c1_lock(timeout_ms)) return false;
    portENTER_CRITICAL(&s_flash_time_mux);
    s_flash_started = xTaskGetTickCount();
    s_flash_active = true;
    portEXIT_CRITICAL(&s_flash_time_mux);
    quiet_idle_interrupts();
    return true;
}

void metalio_i2c1_unlock(void) {
    if (s_mu != NULL) {
        quiet_idle_interrupts();
        portENTER_CRITICAL(&s_flash_time_mux);
        if (s_flash_active) {
            s_flash_ticks += xTaskGetTickCount() - s_flash_started;
            s_flash_active = false;
        }
        portEXIT_CRITICAL(&s_flash_time_mux);
        xSemaphoreGive(s_mu);
    }
}

/* 从设备半掉电可能钳住 SDA；legacy i2c clear_bus 也解不开时需 bit-bang */
static void recover_i2c_bus_pins(gpio_num_t sda, gpio_num_t scl) {
    const gpio_config_t conf = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(20);

    for (int i = 0; i < 9 && gpio_get_level(sda) == 0; ++i) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);
}

esp_err_t metalio_i2c1_init(gpio_num_t sda, gpio_num_t scl) {
    if (!metalio_i2c1_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (s_bus != NULL) {
        err = (s_sda == sda && s_scl == scl) ? ESP_OK : ESP_ERR_INVALID_STATE;
    } else {
        recover_i2c_bus_pins(sda, scl);
        const i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t created = NULL;
        err = i2c_new_master_bus(&config, &created);
        if (err == ESP_OK) {
            s_bus = created;
            s_sda = sda; s_scl = scl;
            ESP_LOGI("MetalioI2C", "new master I2C1 ready SDA=%d SCL=%d 100kHz synchronous", sda, scl);
        }
    }
    metalio_i2c1_unlock();
    return err;
}

esp_err_t metalio_i2c1_deinit(void) {
    if (!metalio_i2c1_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (s_bus != NULL) {
        for (int i = 0; i < DEVICE_SLOTS; ++i) {
            if (s_devices[i].handle == NULL) continue;
            err = i2c_master_bus_rm_device(s_devices[i].handle);
            if (err != ESP_OK) break;
            s_devices[i].handle = NULL;
        }
        if (err == ESP_OK) {
            quiet_idle_interrupts();
            err = i2c_del_master_bus(s_bus);
            if (err == ESP_OK) {
                s_bus = NULL;
            }
        }
    }
    metalio_i2c1_unlock();
    return err;
}

esp_err_t metalio_i2c1_device_locked(i2c_port_t port, uint8_t addr,
                                     i2c_master_dev_handle_t* device) {
    if (device == NULL || addr > 0x7f || port != I2C_NUM_1) return ESP_ERR_INVALID_ARG;
    *device = NULL;
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    int free_slot = -1;
    for (int i = 0; i < DEVICE_SLOTS; ++i) {
        if (s_devices[i].handle != NULL && s_devices[i].addr == addr) {
            *device = s_devices[i].handle;
            return ESP_OK;
        }
        if (s_devices[i].handle == NULL && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return ESP_ERR_NO_MEM;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
        .scl_wait_us = 10000, // BQ27220 can stretch >4ms; do not use the short default.
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &config, device);
    if (err == ESP_OK) {
        s_devices[free_slot].addr = addr;
        s_devices[free_slot].handle = *device;
    }
    return err;
}

esp_err_t metalio_i2c1_probe_locked(i2c_port_t port, uint8_t addr, int timeout_ms) {
    if (port != I2C_NUM_1 || addr > 0x7f) return ESP_ERR_INVALID_ARG;
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(s_bus, addr, timeout_ms);
}

esp_err_t metalio_i2c1_transmit_locked(i2c_port_t port, uint8_t addr,
                                       const uint8_t* data, size_t len, int timeout_ms) {
    i2c_master_dev_handle_t device;
    esp_err_t err = metalio_i2c1_device_locked(port, addr, &device);
    return err == ESP_OK ? i2c_master_transmit(device, data, len, timeout_ms) : err;
}

esp_err_t metalio_i2c1_receive_locked(i2c_port_t port, uint8_t addr,
                                      uint8_t* data, size_t len, int timeout_ms) {
    i2c_master_dev_handle_t device;
    esp_err_t err = metalio_i2c1_device_locked(port, addr, &device);
    return err == ESP_OK ? i2c_master_receive(device, data, len, timeout_ms) : err;
}

esp_err_t metalio_i2c1_transmit_receive_locked(i2c_port_t port, uint8_t addr,
    const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_len, int timeout_ms) {
    i2c_master_dev_handle_t device;
    esp_err_t err = metalio_i2c1_device_locked(port, addr, &device);
    return err == ESP_OK ? i2c_master_transmit_receive(device, tx, tx_len, rx, rx_len, timeout_ms) : err;
}

esp_err_t metalio_i2c1_write_reg_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    const uint8_t* data, size_t len, int timeout_ms) {
    if (len != 0 && data == NULL) return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t device;
    esp_err_t err = metalio_i2c1_device_locked(port, addr, &device);
    if (err != ESP_OK) return err;
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = &reg, .buffer_size = 1},
        {.write_buffer = data, .buffer_size = len},
    };
    return i2c_master_multi_buffer_transmit(device, buffers, len ? 2 : 1, timeout_ms);
}

esp_err_t metalio_i2c1_read_reg_stop_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    uint8_t* data, size_t len, int timeout_ms) {
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = metalio_i2c1_transmit_locked(port, addr, &reg, 1, timeout_ms);
    if (err == ESP_OK) err = metalio_i2c1_receive_locked(port, addr, data, len, timeout_ms);
    return err;
}
