#include "metalio_power.h"

#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "metalio_i2c1.h"
#include "settings.h"
#include <atomic>
#include <esp_timer.h>

#include <driver/i2c_master.h>
#include <esp_log.h>

#define TAG "MetalioPower"

#define EPDIY_I2C_PORT I2C_NUM_1
#define POWER_I2C_LOCK_MS 100

namespace {

std::atomic<int> s_current_ma{1000};
esp_timer_handle_t s_save_timer = nullptr;
bool ValidCurrent(int ma) { return ma == 500 || ma == 1000 || ma == 1500; }
void SaveCurrent(void*) {
    Settings settings("charger", true);
    settings.SetInt("current_ma", s_current_ma.load());
}

power_i2c_t MakeBus() {
    power_i2c_t bus = {
        .port = EPDIY_I2C_PORT,
        .lock = metalio_i2c1_lock,
        .unlock = metalio_i2c1_unlock,
        .device = metalio_i2c1_device_locked,
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

    Settings settings("charger");
    const int saved_ma = settings.GetInt("current_ma", 1000);
    const int current_ma = ValidCurrent(saved_ma) ? saved_ma : 1000;
    err = cx25601n_set_ichg_ma(current_ma);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N set ichg=%lu failed: %s",
                 static_cast<unsigned long>(current_ma), esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CX25601N ready @0x%02X, ichg=%lu mA", CX25601N_I2C_ADDR,
                 static_cast<unsigned long>(current_ma));
    }

    if (err != ESP_OK) return;
    s_current_ma.store(current_ma);
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = SaveCurrent;
    timer_args.name = "charge_save";
    err = esp_timer_create(&timer_args, &s_save_timer);
    if (err != ESP_OK) ESP_LOGW(TAG, "Create save timer failed: %s", esp_err_to_name(err));

    err = cx25601n_enable_charge(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N enable charge failed: %s", esp_err_to_name(err));
    }
}

int MetalioPower_GetChargeCurrentMa(void) {
    return s_current_ma.load();
}

bool MetalioPower_SetChargeCurrentMa(int ma) {
    if (!ValidCurrent(ma) || !cx25601n_is_ready() || s_save_timer == nullptr) return false;
    const int previous_ma = s_current_ma.load();
    if (ma == previous_ma) return true;
    const esp_err_t err = cx25601n_set_ichg_ma(ma);
    if (err != ESP_OK) {
        const esp_err_t restore_err = cx25601n_set_ichg_ma(previous_ma);
        ESP_LOGW(TAG, "Set charge current failed: %s; restore: %s",
                 esp_err_to_name(err), esp_err_to_name(restore_err));
        return false;
    }
    s_current_ma.store(ma);
    // LVGL 栈位于 PSRAM，NVS 写入交给内部 RAM 栈的定时器任务。
    esp_timer_stop(s_save_timer);
    return esp_timer_start_once(s_save_timer, 200000) == ESP_OK;
}
