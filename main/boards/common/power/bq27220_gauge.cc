#include "bq27220_gauge.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Bq27220Gauge"

namespace {
constexpr uint8_t  kRegVoltage   = 0x08;
constexpr uint8_t  kRegCurrent   = 0x0C;
constexpr int      kI2cTimeoutMs = 200; // 含时钟拉伸
constexpr int      kProbeTimeoutMs = 100;
constexpr float    kBatteryEmptyV = 3.3f;
constexpr float    kBatteryFullV  = 4.33f; // 显示满电；充电截止见 CX25601N VREG 4400mV
} // namespace

bool Bq27220Gauge::Begin(const power_i2c_t* bus, uint8_t addr) {
    if (bus == NULL) {
        ESP_LOGW(TAG, "Begin() called with null bus");
        return false;
    }
    bus_ = *bus;
    bus_valid_ = true;
    addr_ = addr;
    if (ready_) {
        return true;
    }

    esp_err_t probe = power_i2c_probe(&bus_, addr_, kProbeTimeoutMs);
    if (probe != ESP_OK) {
        static bool warned = false;
        if (!warned) {
            ESP_LOGW(TAG,
                     "BQ27220 @0x%02X probe NACK (err=0x%x)，"
                     "暂不显示电量；插上电池/上电后会自动重试",
                     addr_, probe);
            warned = true;
        }
        return false;
    }

    // 仅电压读通才算 ready。手册：标准命令 ≤2 次/秒，间隔至少 500ms
    uint16_t mv = 0;
    for (int i = 0; i < 2; ++i) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (ReadU16(kRegVoltage, &mv) && mv >= 2500 && mv <= 5000) {
            ready_ = true;
            ESP_LOGI(TAG, "BQ27220 online @0x%02X, voltage=%u mV", addr_, mv);
            return true;
        }
    }
    ESP_LOGW(TAG, "BQ27220 @0x%02X ACK 但读电压失败，稍后重试", addr_);
    return false;
}

bool Bq27220Gauge::ReadVoltageMv(uint16_t& mv) {
    if (!bus_valid_) {
        return false;
    }
    if (!ready_ && !Begin(&bus_, addr_)) {
        return false;
    }
    return ReadU16(kRegVoltage, &mv);
}

bool Bq27220Gauge::ReadCurrentMa(int16_t& current_ma) {
    if (!bus_valid_) {
        return false;
    }
    if (!ready_ && !Begin(&bus_, addr_)) {
        return false;
    }
    uint16_t raw = 0;
    if (!ReadU16(kRegCurrent, &raw)) {
        return false;
    }
    current_ma = static_cast<int16_t>(raw);
    return true;
}

int Bq27220Gauge::ApplyMonoStep(int target_pct, bool charging) {
    if (target_pct < 0) {
        target_pct = 0;
    }
    if (target_pct > 100) {
        target_pct = 100;
    }

    const int64_t now = esp_timer_get_time();
    if (displayed_soc_ < 0) {
        displayed_soc_ = target_pct;
        last_soc_step_us_ = now;
        return displayed_soc_;
    }

    if (now - last_soc_step_us_ < kSocStepUs) {
        return displayed_soc_;
    }

    if (charging) {
        if (target_pct > displayed_soc_) {
            ++displayed_soc_;
            last_soc_step_us_ = now;
        }
    } else {
        if (target_pct < displayed_soc_) {
            --displayed_soc_;
            last_soc_step_us_ = now;
        }
    }
    return displayed_soc_;
}

bool Bq27220Gauge::GetBatteryLevel(int& level, bool& charging, bool& discharging) {
    if (!ready_) {
        if (++retry_counter_ % 10 != 1) {
            return false;
        }
        if (!bus_valid_ || !Begin(&bus_, addr_)) {
            return false;
        }
    }

    uint16_t mv = 0;
    if (!ReadU16(kRegVoltage, &mv)) {
        return false;
    }
    float bat_v = static_cast<float>(mv) / 1000.0f;

    float raw_pct;
    if (bat_v >= kBatteryFullV) {
        raw_pct = 100.0f;
    } else if (bat_v <= kBatteryEmptyV) {
        raw_pct = 0.0f;
    } else {
        raw_pct = (bat_v - kBatteryEmptyV) / (kBatteryFullV - kBatteryEmptyV) * 100.0f;
    }
    float smoothed = FilterPush(raw_pct);
    int target = static_cast<int>(smoothed + 0.5f);
    if (target < 0) {
        target = 0;
    }
    if (target > 100) {
        target = 100;
    }

    const int64_t now = esp_timer_get_time();
    bool full_ok = false;
    if (mv >= kFullConfirmMv) {
        if (full_above_since_us_ == 0) {
            full_above_since_us_ = now;
        }
        full_ok = (now - full_above_since_us_ >= kFullHoldUs);
    } else {
        full_above_since_us_ = 0;
    }
    if (target >= 100 && !full_ok) {
        target = 99;
    }
    if (displayed_soc_ >= 100 && mv >= kFullExitMv) {
        target = 100;
    }

    int16_t current_ma = 0;
    ReadCurrentMa(current_ma);
    const bool raw_charging = (current_ma > 5);
    discharging = (current_ma < -5);
    const int charge_dir = raw_charging ? 1 : (discharging ? -1 : 0);

    if (displayed_soc_ >= 0 && charge_dir != last_charge_dir_) {
        last_soc_step_us_ = now;
        last_charge_dir_ = charge_dir;
    } else if (displayed_soc_ < 0) {
        last_charge_dir_ = charge_dir;
    }

    level = ApplyMonoStep(target, raw_charging);
    if (full_ok && target >= 100 && displayed_soc_ >= 99) {
        displayed_soc_ = 100;
        level = 100;
    }
    charging = raw_charging && (level < 100);
    return true;
}

void Bq27220Gauge::ResetFilter() {
    filter_idx_ = 0;
    filter_count_ = 0;
    filter_sum_ = 0.0f;
    filter_primed_ = false;
    displayed_soc_ = -1;
    last_soc_step_us_ = 0;
    full_above_since_us_ = 0;
    last_charge_dir_ = 0;
}

bool Bq27220Gauge::ReadU16(uint8_t reg, uint16_t* out) {
    if (!bus_valid_ || out == nullptr) {
        return false;
    }
    uint8_t buf[2] = {0};
    esp_err_t err = power_i2c_write_read(&bus_, addr_, reg, buf, sizeof(buf), kI2cTimeoutMs);
    if (err != ESP_OK) {
        if (++consecutive_err_ % 10 == 1) {
            ESP_LOGW(TAG, "BQ27220 read reg 0x%02X failed: 0x%x (consecutive=%d)", reg, err,
                     consecutive_err_);
        }
        // 连续失败则降 ready，交给 GetBatteryLevel 节流重挂
        if (consecutive_err_ >= 5) {
            ready_ = false;
        }
        return false;
    }
    consecutive_err_ = 0;
    *out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
    return true;
}

float Bq27220Gauge::FilterPush(float sample) {
    if (!filter_primed_) {
        for (int i = 0; i < kFilterSize; ++i) {
            filter_buf_[i] = sample;
        }
        filter_sum_ = sample * kFilterSize;
        filter_count_ = kFilterSize;
        filter_idx_ = 0;
        filter_primed_ = true;
        return sample;
    }
    filter_sum_ -= filter_buf_[filter_idx_];
    filter_buf_[filter_idx_] = sample;
    filter_sum_ += sample;
    filter_idx_ = (filter_idx_ + 1) % kFilterSize;
    if (filter_count_ < kFilterSize) {
        filter_count_++;
    }
    return filter_sum_ / filter_count_;
}

extern "C" signed int battery_get_bat_voltage(void) {
    uint16_t mv = 0;
    if (!Bq27220Gauge::GetInstance().ReadVoltageMv(mv)) {
        return 0;
    }
    return static_cast<signed int>(mv);
}

extern "C" signed int battery_get_bat_current(void) {
    int16_t ma = 0;
    if (!Bq27220Gauge::GetInstance().ReadCurrentMa(ma)) {
        return 0;
    }
    return static_cast<signed int>(ma) * 10000;
}
