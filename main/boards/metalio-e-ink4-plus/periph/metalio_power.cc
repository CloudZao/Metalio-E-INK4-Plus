#include "metalio_power.h"

#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "metalio_i2c1.h"

#include <driver/i2c.h>
#include <esp_log.h>

#define TAG "MetalioPower"

#define EPDIY_I2C_PORT I2C_NUM_1
#define POWER_I2C_LOCK_MS 100

namespace {

power_i2c_t MakeBus() {
    power_i2c_t bus = {
        .port = EPDIY_I2C_PORT,
        .lock = metalio_i2c1_lock,
        .unlock = metalio_i2c1_unlock,
    };
    return bus;
}

} // namespace

void MetalioPower_Init(void) {
    power_i2c_t bus = MakeBus();

    // 电量计先挂上，充电任务依赖 battery_get_bat_* 
    Bq27220Gauge::GetInstance().Begin(&bus);

    esp_err_t probe = power_i2c_probe(&bus, CX25601N_I2C_ADDR, POWER_I2C_LOCK_MS);
    if (probe != ESP_OK) {
        ESP_LOGI(TAG, "CX25601N not found at 0x%02X, skip charger init", CX25601N_I2C_ADDR);
        return;
    }

    esp_err_t err = cx25601n_init(&bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N init failed: %s", esp_err_to_name(err));
        return;
    }

    // 板级默认快速充电 1000mA（与 397 设置项默认一致）
    constexpr uint32_t kDefaultIchgMa = 1000;
    err = cx25601n_set_ichg_ma(kDefaultIchgMa);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N set ichg=%lu failed: %s",
                 static_cast<unsigned long>(kDefaultIchgMa), esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CX25601N ready @0x%02X, ichg=%lu mA", CX25601N_I2C_ADDR,
                 static_cast<unsigned long>(kDefaultIchgMa));
    }

    err = cx25601n_enable_charge(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N enable charge failed: %s", esp_err_to_name(err));
    }
}
