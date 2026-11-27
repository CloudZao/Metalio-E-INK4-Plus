#include <atomic>
#include <mutex>
#include <new>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "charger_control.h"
#include "cx25601n.h"
#include "infra/storage/settings.h"

#define TAG "ChargerControl"

#define POWER_I2C_LOCK_MS 100

static const char* s_settings_namespace = nullptr;
static const char* s_settings_key = nullptr;

static std::atomic<int> s_current_ma{BOARD_CHARGE_CURRENT_DEFAULT_MA};
static esp_timer_handle_t s_save_timer = nullptr;
static std::mutex s_save_mutex;       // 保护电流切换与待保存快照
static bool s_save_pending = false;   // 写入失败后保留，后续同档位请求仍会重试
static unsigned s_save_failures = 0;  // 连续失败最多自动重试三次

static bool ValidCurrent(int ma) {
    for (int option : kChargeCurrentOptionsMa) {
        if (ma == option) {
            return true;
        }
    }
    return false;
}

static esp_err_t RestartSaveTimerLocked(uint64_t delay_us) {
    esp_timer_stop(s_save_timer);
    return esp_timer_start_once(s_save_timer, delay_us);
}

static void SaveCurrent(void*) {
    int current_ma;
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        if (!s_save_pending) {
            return;
        }
        current_ma = s_current_ma.load();
    }

    bool saved = false;
    try {
        Settings settings(s_settings_namespace, true);
        saved = settings.SetInt(s_settings_key, current_ma) && settings.Commit();
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "charge preference save failed stage=allocate");
    }

    std::lock_guard<std::mutex> lock(s_save_mutex);
    if (current_ma != s_current_ma.load()) {
        // 新请求已重新启动定时器；本次写入不能清除它的待保存状态。
        return;
    }
    if (saved) {
        s_save_pending = false;
        s_save_failures = 0;
        return;
    }
    if (++s_save_failures < 3) {
        const esp_err_t err = RestartSaveTimerLocked(250000 * s_save_failures);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "charge preference save failed stage=retry_schedule code=0x%x", static_cast<unsigned>(err));
        }
    }
}

void ChargerControl_Init(const power_i2c_t* bus, const char* settings_namespace, const char* settings_key) {
    s_settings_namespace = settings_namespace;
    s_settings_key = settings_key;
    esp_err_t probe = power_i2c_probe(bus, CX25601N_I2C_ADDR, POWER_I2C_LOCK_MS);
    if (probe != ESP_OK) {
        ESP_LOGI(TAG, "CX25601N not found at 0x%02X, skip charger init", CX25601N_I2C_ADDR);
        return;
    }

    esp_err_t err = cx25601n_init(bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N init failed: %s", esp_err_to_name(err));
        return;
    }

    Settings settings(s_settings_namespace);
    const int saved_ma = settings.GetInt(s_settings_key, BOARD_CHARGE_CURRENT_DEFAULT_MA);
    const int current_ma = ValidCurrent(saved_ma) ? saved_ma : BOARD_CHARGE_CURRENT_DEFAULT_MA;
    err = cx25601n_set_ichg_ma(current_ma);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N set ichg=%lu failed: %s", static_cast<unsigned long>(current_ma), esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CX25601N ready @0x%02X, ichg=%lu mA", CX25601N_I2C_ADDR, static_cast<unsigned long>(current_ma));
    }

    if (err != ESP_OK) {
        return;
    }
    s_current_ma.store(current_ma);
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = SaveCurrent;
    timer_args.name = "charge_save";
    err = esp_timer_create(&timer_args, &s_save_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Create save timer failed: %s", esp_err_to_name(err));
    }

    err = cx25601n_enable_charge(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CX25601N enable charge failed: %s", esp_err_to_name(err));
    }
}

int ChargerControl_GetChargeCurrentMa(void) {
    return s_current_ma.load();
}

bool ChargerControl_SetChargeCurrentMa(int ma) {
    std::lock_guard<std::mutex> lock(s_save_mutex);
    if (!ValidCurrent(ma) || !cx25601n_is_ready() || s_save_timer == nullptr) {
        return false;
    }
    const int previous_ma = s_current_ma.load();
    if (ma == previous_ma && !s_save_pending) {
        return true;
    }
    if (ma != previous_ma) {
        const esp_err_t err = cx25601n_set_ichg_ma(ma);
        if (err != ESP_OK) {
            const esp_err_t restore_err = cx25601n_set_ichg_ma(previous_ma);
            ESP_LOGW(TAG, "charge current update failed code=0x%x restore_code=0x%x", static_cast<unsigned>(err),
                     static_cast<unsigned>(restore_err));
            return false;
        }
        s_current_ma.store(ma);
    }
    // LVGL 栈位于 PSRAM，NVS 写入交给内部 RAM 栈的定时器任务。
    s_save_pending = true;
    s_save_failures = 0;
    const esp_err_t err = RestartSaveTimerLocked(200000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "charge preference save failed stage=schedule code=0x%x", static_cast<unsigned>(err));
        return false;
    }
    return true;
}
