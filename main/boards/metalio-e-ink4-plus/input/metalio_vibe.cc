/**
 * @file metalio_vibe.cc
 * @brief 震动马达：TCA P0.3，强度 0–3（软 PWM），短震 / 连续
 */

#include "metalio_vibe.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "MetalioVibe"

#define VIBE_PWM_PERIOD_US 20000ULL
static const uint32_t k_duty_on_us[4] = {0, 14000, 18000, 20000};

namespace {

class VibeEngine {
public:
    static VibeEngine& Instance() {
        static VibeEngine inst;
        return inst;
    }

    bool Init() {
        if (initialized_) {
            return true;
        }
        if (!epd_board_metalio_eink4_plus_ioexp_ok()) {
            ESP_LOGE(TAG, "init fail: no TCA");
            return false;
        }
        intensity_ = 1;
        continuous_ = false;
        pulse_active_ = false;
        epd_board_metalio_eink4_plus_motor_set(false);
        initialized_ = true;
        ESP_LOGI(TAG, "init ok TCA P%d pulse=%dms intensity=1", EPD_IO_MOTOR,
                 VIBRATION_MOTOR_PULSE_MS);
        return true;
    }

    void SetIntensity(uint8_t level) {
        if (level > 3) {
            level = 3;
        }
        intensity_ = level;
        if (!initialized_) {
            return;
        }
        if (continuous_ || pulse_active_) {
            ApplyActiveDrive();
        } else if (level == 0) {
            StopOutput();
        }
        ESP_LOGI(TAG, "intensity=%u", (unsigned)intensity_);
    }

    uint8_t intensity() const { return intensity_; }

    void Pulse() {
        if (!initialized_ || continuous_ || intensity_ == 0 || pulse_timer_ == nullptr) {
            return;
        }
        pulse_active_ = true;
        ApplyActiveDrive();
        esp_timer_stop(pulse_timer_);
        esp_err_t err = esp_timer_start_once(
            pulse_timer_, static_cast<uint64_t>(VIBRATION_MOTOR_PULSE_MS) * 1000ULL);
        if (err != ESP_OK) {
            pulse_active_ = false;
            StopOutput();
            ESP_LOGW(TAG, "pulse timer: %s", esp_err_to_name(err));
        }
    }

    void Set(bool on) {
        if (!initialized_) {
            return;
        }
        if (pulse_timer_ != nullptr) {
            esp_timer_stop(pulse_timer_);
        }
        pulse_active_ = false;
        continuous_ = on && intensity_ != 0;
        if (continuous_) {
            ApplyActiveDrive();
        } else {
            continuous_ = false;
            StopOutput();
        }
        ESP_LOGI(TAG, "continuous=%d intensity=%u", continuous_ ? 1 : 0, (unsigned)intensity_);
    }

private:
    VibeEngine() {
        const esp_timer_create_args_t pulse_args = {
            .callback =
                [](void* arg) {
                    static_cast<VibeEngine*>(arg)->OnPulseEnd();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vibe_pulse",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&pulse_args, &pulse_timer_));

        const esp_timer_create_args_t pwm_args = {
            .callback =
                [](void* arg) {
                    static_cast<VibeEngine*>(arg)->OnSoftPwm();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vibe_pwm",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&pwm_args, &pwm_timer_));
    }

    void StopOutput() {
        if (pwm_timer_ != nullptr) {
            esp_timer_stop(pwm_timer_);
        }
        pwm_phase_on_ = false;
        if (epd_board_metalio_eink4_plus_ioexp_ok()) {
            epd_board_metalio_eink4_plus_motor_set(false);
        }
    }

    void ApplyActiveDrive() {
        if (intensity_ == 0) {
            StopOutput();
            return;
        }
        if (intensity_ == 3) {
            if (pwm_timer_ != nullptr) {
                esp_timer_stop(pwm_timer_);
            }
            pwm_phase_on_ = false;
            epd_board_metalio_eink4_plus_motor_set(true);
            return;
        }
        pwm_phase_on_ = true;
        epd_board_metalio_eink4_plus_motor_set(true);
        if (pwm_timer_ != nullptr) {
            esp_timer_stop(pwm_timer_);
            esp_timer_start_once(pwm_timer_, k_duty_on_us[intensity_]);
        }
    }

    void OnPulseEnd() {
        pulse_active_ = false;
        if (!continuous_) {
            StopOutput();
        }
    }

    void OnSoftPwm() {
        if (!initialized_ || intensity_ == 0 || intensity_ == 3) {
            return;
        }
        if (!continuous_ && !pulse_active_) {
            StopOutput();
            return;
        }
        pwm_phase_on_ = !pwm_phase_on_;
        epd_board_metalio_eink4_plus_motor_set(pwm_phase_on_);
        const uint32_t on_us = k_duty_on_us[intensity_];
        const uint32_t off_us = static_cast<uint32_t>(VIBE_PWM_PERIOD_US) - on_us;
        const uint64_t next_us = pwm_phase_on_ ? on_us : off_us;
        if (next_us == 0 || pwm_timer_ == nullptr) {
            return;
        }
        esp_timer_start_once(pwm_timer_, next_us);
    }

    esp_timer_handle_t pulse_timer_ = nullptr;
    esp_timer_handle_t pwm_timer_ = nullptr;
    uint8_t intensity_ = 1;
    bool continuous_ = false;
    bool pulse_active_ = false;
    bool pwm_phase_on_ = false;
    bool initialized_ = false;
};

} // namespace

extern "C" void MetalioVibe_Init(void) {
    VibeEngine::Instance().Init();
}

extern "C" void MetalioVibe_Pulse(void) {
    VibeEngine::Instance().Pulse();
}

extern "C" void MetalioVibe_Set(bool on) {
    VibeEngine::Instance().Set(on);
}

extern "C" void MetalioVibe_SetIntensity(uint8_t level) {
    VibeEngine::Instance().SetIntensity(level);
}

extern "C" uint8_t MetalioVibe_GetIntensity(void) {
    return VibeEngine::Instance().intensity();
}
