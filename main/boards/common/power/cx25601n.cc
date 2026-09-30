/*
 * CX25601N charger driver — ESP-IDF port (legacy I2C)。
 * 寄存器与策略对齐 397 / MTK 参考；含欠压 BATFET 保护与再充 kick。
 */

#include "cx25601n.h"

#include "bq27220_gauge.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "cx25601n";

constexpr uint8_t REG_ICHG_LO    = 0x02;
constexpr uint8_t REG_ICHG_HI    = 0x03;
constexpr uint8_t REG_VREG_LO    = 0x04;
constexpr uint8_t REG_VREG_HI    = 0x05;
constexpr uint8_t REG_IINDPM_LO  = 0x06;
constexpr uint8_t REG_IINDPM_HI  = 0x07;
constexpr uint8_t REG_IOTG_LO    = 0x0A;
constexpr uint8_t REG_IOTG_HI    = 0x0B;
constexpr uint8_t REG_VOTG_LO    = 0x0C;
constexpr uint8_t REG_VOTG_HI    = 0x0D;
constexpr uint8_t REG_IPRECHG_LO = 0x10;
constexpr uint8_t REG_IPRECHG_HI = 0x11;
constexpr uint8_t REG_ITERM_LO   = 0x12;
constexpr uint8_t REG_ITERM_HI   = 0x13;
constexpr uint8_t REG_CHG_CTRL0  = 0x14;
constexpr uint8_t REG_CHG_TMR    = 0x15;
constexpr uint8_t REG_CHG_CTRL1  = 0x16;
constexpr uint8_t REG_CHG_CTRL3  = 0x18;
constexpr uint8_t REG_PART_INFO  = 0x38;
constexpr uint8_t REG_STATUS1    = 0x1E;
constexpr uint8_t REG_UNLOCK     = 0x70;

constexpr int I2C_TIMEOUT_MS = 100;
constexpr int OTG_ENTRY_DELAY_MS = 30;
constexpr int32_t kUvBatfetShutdownMv = 3100; // 无适配器且 ≤3.1V → BATFET Shutdown

power_i2c_t s_bus{};
bool s_bus_valid = false;
SemaphoreHandle_t s_lock = nullptr;
bool s_ready = false;
TaskHandle_t s_vreg_task = nullptr;
bool s_vreg_suspended = false;
uint32_t s_vreg_vol_mv = 0;
uint32_t s_vreg_now_mv = 0;
bool s_uv_shutdown_done = false;

esp_err_t read_byte(uint8_t reg, uint8_t *val) {
    return power_i2c_write_read(&s_bus, CX25601N_I2C_ADDR, reg, val, 1, I2C_TIMEOUT_MS);
}

esp_err_t write_byte(uint8_t reg, uint8_t val) {
    return power_i2c_write_reg(&s_bus, CX25601N_I2C_ADDR, reg, &val, 1, I2C_TIMEOUT_MS);
}

esp_err_t update_bits(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t field) {
    uint8_t cur = 0;
    esp_err_t err = read_byte(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t m = static_cast<uint8_t>(mask << shift);
    cur = static_cast<uint8_t>((cur & static_cast<uint8_t>(~m)) | ((field << shift) & m));
    return write_byte(reg, cur);
}

esp_err_t read_bits(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t *field) {
    uint8_t cur = 0;
    esp_err_t err = read_byte(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    *field = static_cast<uint8_t>((cur >> shift) & mask);
    return ESP_OK;
}

esp_err_t set_vreg_hw_mv(uint32_t mv) {
    if (!s_bus_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mv < CX25601N_VREG_MIN_MV) {
        mv = CX25601N_VREG_MIN_MV;
    }
    if (mv > CX25601N_VREG_MAX_MV) {
        mv = CX25601N_VREG_MAX_MV;
    }
    uint32_t code = mv / CX25601N_VREG_STEP_MV;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = update_bits(REG_VREG_LO, 0x1F, 3, static_cast<uint8_t>(code & 0x1F));
    if (err == ESP_OK) {
        err = update_bits(REG_VREG_HI, 0x0F, 0, static_cast<uint8_t>((code >> 5) & 0x0F));
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set VREG=%lu mV (code=%lu)",
                 static_cast<unsigned long>(code * CX25601N_VREG_STEP_MV),
                 static_cast<unsigned long>(code));
    }
    return err;
}

int32_t ibat_to_ma(int32_t ibat_scaled) {
    int32_t ma = ibat_scaled / 1000;
    return ma > 0 ? ma : 0;
}

void apply_vreg_fixed(uint32_t mv) {
    if (mv < CX25601N_VREG_MIN_MV) {
        mv = CX25601N_VREG_MIN_MV;
    }
    if (mv > CX25601N_VREG_MAX_MV) {
        mv = CX25601N_VREG_MAX_MV;
    }
    if (mv == s_vreg_now_mv) {
        return;
    }
    if (set_vreg_hw_mv(mv) == ESP_OK) {
        s_vreg_now_mv = mv;
    }
}

void set_en_term(bool enable) {
    update_bits(REG_CHG_CTRL0, 0x01, 2, enable ? 1 : 0);
}

void kick_recharge(int32_t vbat, uint32_t vreg_target) {
    ESP_LOGI(TAG, "recharge kick: vbat=%ldmV < VREG=%lumV", static_cast<long>(vbat),
             static_cast<unsigned long>(vreg_target));

    apply_vreg_fixed(vreg_target);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_en_term(false);
    update_bits(REG_CHG_CTRL1, 0x01, 5, 0);
    update_bits(REG_CHG_CTRL1, 0x01, 4, 0);
    xSemaphoreGive(s_lock);

    vTaskDelay(pdMS_TO_TICKS(200));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_bits(REG_CHG_CTRL1, 0x01, 4, 0);
    update_bits(REG_CHG_CTRL1, 0x01, 5, 1);
    xSemaphoreGive(s_lock);

    vTaskDelay(pdMS_TO_TICKS(500));

    uint8_t stat = 0;
    const bool ok = (cx25601n_get_chrg_stat(&stat) == ESP_OK);
    ESP_LOGI(TAG, "recharge after: stat=%u (%s)", static_cast<unsigned>(stat),
             ok ? cx25601n_chrg_stat_str(stat) : "read fail");
}

void charger_vreg_task(void *arg) {
    (void)arg;

    while (true) {
        const uint32_t vreg_target = s_vreg_vol_mv;
        const int32_t vbat_peek = battery_get_bat_voltage();
        const bool below_cutoff =
            (vreg_target >= CX25601N_VREG_MIN_MV && vreg_target <= CX25601N_VREG_MAX_MV &&
             vbat_peek >= 2500 && static_cast<uint32_t>(vbat_peek) < vreg_target);
        vTaskDelay(pdMS_TO_TICKS(below_cutoff ? 2000 : 10000));

        if (vreg_target < CX25601N_VREG_MIN_MV || vreg_target > CX25601N_VREG_MAX_MV) {
            continue;
        }

        apply_vreg_fixed(vreg_target);

        const int32_t vbat = battery_get_bat_voltage();
        const int32_t ima = ibat_to_ma(battery_get_bat_current() / 10);
        uint8_t stat = 0;
        const bool have_stat = (cx25601n_get_chrg_stat(&stat) == ESP_OK);
        const bool idle = have_stat && (stat == CX25601N_CHG_STAT_NOT);
        const bool vbat_below = (vbat >= 2500 && static_cast<uint32_t>(vbat) < vreg_target);

        // 欠压保护：无输入且 VBAT≤3.1V → REG0x18=0x01 关 BATFET
        if (!s_uv_shutdown_done && vbat >= 2500 && vbat <= kUvBatfetShutdownMv) {
            uint8_t vbus = 0;
            const bool have_vbus = (cx25601n_get_vbus_stat(&vbus) == ESP_OK);
            if (have_vbus && vbus == 0) {
                ESP_LOGW(TAG, "UV protect: vbat=%ldmV → REG0x18=0x01 (BATFET shutdown)",
                         static_cast<long>(vbat));
                if (cx25601n_enter_batfet_shutdown() == ESP_OK) {
                    s_uv_shutdown_done = true;
                }
            }
        } else if (s_uv_shutdown_done && vbat > kUvBatfetShutdownMv + 100) {
            s_uv_shutdown_done = false;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_en_term(!vbat_below);
        xSemaphoreGive(s_lock);

        if (idle && ima < 5 && vbat_below) {
            kick_recharge(vbat, vreg_target);
        }
    }
}

void unlock_private(bool enable) {
    if (!enable) {
        write_byte(REG_UNLOCK, 0x00);
        return;
    }
    for (int i = 0; i < 10; i++) {
        write_byte(REG_UNLOCK, 0x00);
        write_byte(REG_UNLOCK, 0x50);
        write_byte(REG_UNLOCK, 0x57);
        write_byte(REG_UNLOCK, 0x44);
        uint8_t v = 0;
        if (read_byte(REG_UNLOCK, &v) == ESP_OK && v == 0x03) {
            return;
        }
    }
    ESP_LOGW(TAG, "private register unlock failed");
}

esp_err_t set_dis_dpdm(bool disable) {
    return update_bits(REG_CHG_TMR, 0x01, 6, disable ? 0 : 1);
}

esp_err_t set_iindpm_ma_nolock(uint32_t ma) {
    if (ma < CX25601N_IINDPM_MIN_MA) {
        ma = CX25601N_IINDPM_MIN_MA;
    }
    if (ma > CX25601N_IINDPM_MAX_MA) {
        ma = CX25601N_IINDPM_MAX_MA;
    }
    uint32_t code = ma / CX25601N_IINDPM_STEP_MA;
    if (code < 5) {
        code = 5;
    }
    if (code > 150) {
        code = 150;
    }

    esp_err_t err = update_bits(REG_IINDPM_LO, 0x0F, 4, static_cast<uint8_t>(code & 0x0F));
    if (err == ESP_OK) {
        err = update_bits(REG_IINDPM_HI, 0x0F, 0, static_cast<uint8_t>((code >> 4) & 0x0F));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set IINDPM=%lu mA (code=%lu)",
                 static_cast<unsigned long>(code * CX25601N_IINDPM_STEP_MA),
                 static_cast<unsigned long>(code));
    }
    return err;
}

esp_err_t set_votg_mv_nolock(uint32_t mv) {
    if (mv < CX25601N_VOTG_MIN_MV) {
        mv = CX25601N_VOTG_MIN_MV;
    }
    if (mv > CX25601N_VOTG_MAX_MV) {
        mv = CX25601N_VOTG_MAX_MV;
    }
    uint32_t code = mv / CX25601N_VOTG_STEP_MV;
    if (code < 0x30) {
        code = 0x30;
    }
    if (code > 0x42) {
        code = 0x42;
    }

    esp_err_t err = update_bits(REG_VOTG_LO, 0x03, 6, static_cast<uint8_t>(code & 0x03));
    if (err == ESP_OK) {
        err = update_bits(REG_VOTG_HI, 0x1F, 0, static_cast<uint8_t>((code >> 2) & 0x1F));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set VOTG=%lu mV (code=0x%02lX)",
                 static_cast<unsigned long>(code * CX25601N_VOTG_STEP_MV),
                 static_cast<unsigned long>(code));
    }
    return err;
}

esp_err_t set_iotg_ma_nolock(uint32_t ma) {
    if (ma < CX25601N_IOTG_MIN_MA) {
        ma = CX25601N_IOTG_MIN_MA;
    }
    if (ma > CX25601N_IOTG_MAX_MA) {
        ma = CX25601N_IOTG_MAX_MA;
    }
    uint32_t code = ma / CX25601N_IOTG_STEP_MA;
    if (code < 5) {
        code = 5;
    }
    if (code > 60) {
        code = 60;
    }

    esp_err_t err = update_bits(REG_IOTG_LO, 0x0F, 4, static_cast<uint8_t>(code & 0x0F));
    if (err == ESP_OK) {
        err = update_bits(REG_IOTG_HI, 0x0F, 0, static_cast<uint8_t>((code >> 4) & 0x0F));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set IOTG=%lu mA (code=0x%02lX)",
                 static_cast<unsigned long>(code * CX25601N_IOTG_STEP_MA),
                 static_cast<unsigned long>(code));
    }
    return err;
}

esp_err_t hw_init_defaults(void) {
    ESP_RETURN_ON_ERROR(set_dis_dpdm(true), TAG, "dis_dpdm");
    ESP_RETURN_ON_ERROR(update_bits(REG_CHG_CTRL1, 0x01, 4, 0), TAG, "HIZ");
    ESP_RETURN_ON_ERROR(update_bits(REG_CHG_CTRL1, 0x03, 0, 0), TAG, "WDT");
    ESP_RETURN_ON_ERROR(update_bits(REG_IPRECHG_LO, 0x0F, 4, 0x0C), TAG, "iprechg lo");
    ESP_RETURN_ON_ERROR(update_bits(REG_IPRECHG_HI, 0x01, 0, 0x00), TAG, "iprechg hi");
    ESP_RETURN_ON_ERROR(update_bits(REG_ITERM_LO, 0x1F, 3, 0x06), TAG, "iterm lo");
    ESP_RETURN_ON_ERROR(update_bits(REG_ITERM_HI, 0x01, 0, 0x00), TAG, "iterm hi");
    ESP_RETURN_ON_ERROR(cx25601n_set_vreg_mv(4400), TAG, "default vreg");
    ESP_RETURN_ON_ERROR(cx25601n_set_ichg_ma(500), TAG, "default ichg");

    unlock_private(true);
    write_byte(0x86, 0x06);
    write_byte(0x3A, 0x10);
    write_byte(0x46, 0x20);
    unlock_private(false);

    update_bits(REG_CHG_CTRL0, 0x01, 0, 1);
    update_bits(REG_CHG_CTRL3, 0x01, 2, 0);
    update_bits(0x1A, 0x01, 7, 1);
    update_bits(0x23, 0x07, 2, 0x07);
    update_bits(0x24, 0x01, 3, 1);

    ESP_RETURN_ON_ERROR(update_bits(REG_CHG_CTRL1, 0x01, 5, 1), TAG, "EN_CHG");
    ESP_LOGI(TAG, "hw defaults applied");
    return ESP_OK;
}

} // namespace

esp_err_t cx25601n_init(const power_i2c_t* bus) {
    if (s_ready) {
        return ESP_OK;
    }
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_bus = *bus;
    s_bus_valid = true;

    uint8_t part = 0;
    esp_err_t err = read_byte(REG_PART_INFO, &part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe PART_INFO(0x38) failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PART_INFO=0x%02X @0x%02X", part, CX25601N_I2C_ADDR);

    err = hw_init_defaults();
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    if (xTaskCreate(charger_vreg_task, "cx25601n_vreg", 3072, nullptr, 5, &s_vreg_task) != pdPASS) {
        s_ready = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool cx25601n_is_ready(void) {
    return s_ready;
}

void cx25601n_vreg_suspend_for_sleep(void) {
    if (s_vreg_task == nullptr || s_vreg_suspended) {
        return;
    }
    vTaskSuspend(s_vreg_task);
    s_vreg_suspended = true;
    vTaskDelay(pdMS_TO_TICKS(1));
}

void cx25601n_vreg_resume_after_sleep(void) {
    if (s_vreg_task == nullptr || !s_vreg_suspended) {
        return;
    }
    s_vreg_suspended = false;
    vTaskResume(s_vreg_task);
}

esp_err_t cx25601n_enter_batfet_shutdown(void) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = write_byte(REG_CHG_CTRL3, 0x01);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "REG0x18=0x01 (BATFET shutdown)");
    }
    return err;
}

esp_err_t cx25601n_read_reg(uint8_t reg, uint8_t *val) {
    if (!s_ready || !val) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_byte(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_enable_charge(bool enable) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (enable) {
        err = update_bits(REG_CHG_CTRL1, 0x01, 4, 0);
        if (err == ESP_OK) {
            err = update_bits(REG_CHG_CTRL1, 0x01, 5, 1);
        }
    } else {
        err = update_bits(REG_CHG_CTRL1, 0x01, 5, 0);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "charge %s", enable ? "ON" : "OFF");
    return err;
}

esp_err_t cx25601n_is_charge_enabled(bool *enabled) {
    if (!s_ready || !enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bit = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_CHG_CTRL1, 0x01, 5, &bit);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        *enabled = bit != 0;
    }
    return err;
}

esp_err_t cx25601n_enable_otg(bool enable) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (enable) {
        err = update_bits(REG_CHG_CTRL1, 0x01, 5, 0);
        if (err == ESP_OK) {
            err = update_bits(REG_CHG_CTRL1, 0x01, 4, 0);
        }
        if (err == ESP_OK) {
            err = set_votg_mv_nolock(CX25601N_OTG_DEFAULT_MV);
        }
        if (err == ESP_OK) {
            err = set_iotg_ma_nolock(CX25601N_OTG_DEFAULT_MA);
        }
        if (err == ESP_OK) {
            err = update_bits(REG_CHG_CTRL3, 0x01, 6, 1);
        }
        xSemaphoreGive(s_lock);

        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(OTG_ENTRY_DELAY_MS));
            ESP_LOGI(TAG, "OTG ON (target %dmV/%dmA)", CX25601N_OTG_DEFAULT_MV,
                     CX25601N_OTG_DEFAULT_MA);
        } else {
            ESP_LOGE(TAG, "OTG enable failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    err = update_bits(REG_CHG_CTRL3, 0x01, 6, 0);
    if (err == ESP_OK) {
        err = update_bits(REG_CHG_CTRL1, 0x01, 5, 1);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTG OFF (charge restored)");
    } else {
        ESP_LOGE(TAG, "OTG disable failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cx25601n_is_otg_enabled(bool *enabled) {
    if (!s_ready || !enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bit = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_CHG_CTRL3, 0x01, 6, &bit);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        *enabled = bit != 0;
    }
    return err;
}

esp_err_t cx25601n_set_iindpm_ma(uint32_t ma) {
    if (!s_bus_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = set_iindpm_ma_nolock(ma);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_get_iindpm_ma(uint32_t *ma) {
    if (!s_ready || !ma) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_IINDPM_LO, 0x0F, 4, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_IINDPM_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 4);
    *ma = code * CX25601N_IINDPM_STEP_MA;
    return ESP_OK;
}

esp_err_t cx25601n_set_ichg_ma(uint32_t ma) {
    if (!s_bus_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ma < CX25601N_ICHG_MIN_MA) {
        ma = CX25601N_ICHG_MIN_MA;
    }
    if (ma > CX25601N_ICHG_MAX_MA) {
        ma = CX25601N_ICHG_MAX_MA;
    }
    uint32_t code = ma / CX25601N_ICHG_STEP_MA;
    if (code < 1) {
        code = 1;
    }
    if (code > 38) {
        code = 38;
    }
    uint32_t applied_ma = code * CX25601N_ICHG_STEP_MA;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = update_bits(REG_ICHG_LO, 0x03, 6, static_cast<uint8_t>(code & 0x03));
    if (err == ESP_OK) {
        err = update_bits(REG_ICHG_HI, 0x0F, 0, static_cast<uint8_t>((code >> 2) & 0x0F));
    }
    if (err == ESP_OK) {
        err = set_iindpm_ma_nolock(applied_ma);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "set ICHG=%lu mA (code=%lu)", static_cast<unsigned long>(applied_ma),
             static_cast<unsigned long>(code));
    return err;
}

esp_err_t cx25601n_get_ichg_ma(uint32_t *ma) {
    if (!s_ready || !ma) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_ICHG_LO, 0x03, 6, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_ICHG_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 2);
    *ma = code * CX25601N_ICHG_STEP_MA;
    return ESP_OK;
}

esp_err_t cx25601n_set_vreg_mv(uint32_t mv) {
    if (!s_bus_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mv < CX25601N_VREG_MIN_MV) {
        mv = CX25601N_VREG_MIN_MV;
    }
    if (mv > CX25601N_VREG_MAX_MV) {
        mv = CX25601N_VREG_MAX_MV;
    }

    if (s_vreg_vol_mv == mv && s_vreg_now_mv == mv) {
        return ESP_OK;
    }

    esp_err_t err = set_vreg_hw_mv(mv);
    if (err == ESP_OK) {
        s_vreg_vol_mv = mv;
        s_vreg_now_mv = mv;
    }
    return err;
}

esp_err_t cx25601n_get_vreg_mv(uint32_t *mv) {
    if (!s_ready || !mv) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_VREG_LO, 0x1F, 3, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_VREG_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 5);
    *mv = code * CX25601N_VREG_STEP_MV;
    return ESP_OK;
}

esp_err_t cx25601n_get_chrg_stat(uint8_t *stat) {
    if (!s_ready || !stat) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_STATUS1, 0x03, 3, stat);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_get_vbus_stat(uint8_t *stat) {
    if (!s_ready || !stat) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_STATUS1, 0x07, 0, stat);
    xSemaphoreGive(s_lock);
    return err;
}

const char *cx25601n_chrg_stat_str(uint8_t stat) {
    switch (stat) {
    case CX25601N_CHG_STAT_NOT:
        return "未充电/已满";
    case CX25601N_CHG_STAT_CC:
        return "涓流/预充/CC";
    case CX25601N_CHG_STAT_CV:
        return "恒压降流";
    case CX25601N_CHG_STAT_TOPOFF:
        return "Top-off";
    default:
        return "未知";
    }
}

const char *cx25601n_vbus_stat_str(uint8_t stat) {
    switch (stat) {
    case 0:
        return "无输入";
    case 1:
        return "USB SDP";
    case 2:
        return "USB CDP";
    case 3:
        return "USB DCP";
    case 4:
        return "未知适配器";
    case 5:
        return "非标适配器";
    case 7:
        return "OTG";
    default:
        return "适配器";
    }
}
