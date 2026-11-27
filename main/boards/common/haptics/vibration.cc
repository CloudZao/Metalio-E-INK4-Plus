#include <mutex>
#include <esp_log.h>
#include <esp_timer.h>
#include "vibration.h"

static const char* TAG = "Vibration";
static constexpr uint32_t kPeriodUs = 20000;
static constexpr uint32_t kOnUs[] = {0, 14000, 18000, 20000};
static std::recursive_mutex s_mutex;
static VibrationConfig s_config{};
static esp_timer_handle_t s_pulse_timer = nullptr;
static esp_timer_handle_t s_pwm_timer = nullptr;
static int64_t s_pulse_until = 0;
static uint32_t s_pulse_id = 0; // 在同一驱动锁下分配，区分应用脉冲与后续系统反馈
static int64_t s_pwm_due = 0;
static uint8_t s_intensity = 1;
static bool s_initialized = false;
static bool s_continuous = false;
static bool s_phase_on = false;

static esp_err_t WriteOutput(bool on) {
    esp_err_t err;
    if (s_config.output == VibrationOutput::ExpandedPin) {
        err = s_config.write_expanded(on);
    } else {
        err = gpio_set_direction(s_config.gpio, GPIO_MODE_OUTPUT);
        if (err == ESP_OK) {
            err = gpio_set_level(s_config.gpio, on == s_config.active_high);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "output failed on=%d code=%d", on, err);
    }
    return err;
}

static void StopOutput() {
    if (s_pwm_timer) {
        esp_timer_stop(s_pwm_timer);
    }
    s_pwm_due = 0;
    s_phase_on = false;
    WriteOutput(false);
}

static bool ArmTimer(esp_timer_handle_t timer, uint64_t delay_us) {
    esp_timer_stop(timer);
    const esp_err_t err = esp_timer_start_once(timer, delay_us);
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGE(TAG, "timer start failed code=%d", err);
    s_continuous = false;
    s_pulse_until = 0;
    StopOutput();
    return false;
}

static void ApplyDrive() {
    if (s_intensity == 0) {
        StopOutput();
        return;
    }
    s_phase_on = true;
    if (WriteOutput(true) != ESP_OK) {
        s_continuous = false;
        s_pulse_until = 0;
        StopOutput();
        return;
    }
    if (s_config.drive == VibrationDrive::Pwm && s_intensity < 3) {
        s_pwm_due = esp_timer_get_time() + kOnUs[s_intensity];
        ArmTimer(s_pwm_timer, kOnUs[s_intensity]);
    } else if (s_pwm_timer) {
        esp_timer_stop(s_pwm_timer);
        s_pwm_due = 0;
    }
}

static void PulseEnd(void*) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_pulse_until) {
        return;
    }
    const int64_t remaining = s_pulse_until - esp_timer_get_time();
    if (remaining > 0) {
        ArmTimer(s_pulse_timer, remaining);
        return;
    }
    s_pulse_until = 0;
    if (!s_continuous) {
        StopOutput();
    }
}

static void PwmTick(void*) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_pwm_due || (!s_continuous && !s_pulse_until)) {
        return;
    }
    const int64_t remaining = s_pwm_due - esp_timer_get_time();
    if (remaining > 0) {
        ArmTimer(s_pwm_timer, remaining);
        return;
    }
    s_phase_on = !s_phase_on;
    if (WriteOutput(s_phase_on) != ESP_OK) {
        s_continuous = false;
        s_pulse_until = 0;
        StopOutput();
        return;
    }
    const uint32_t delay = s_phase_on ? kOnUs[s_intensity] : kPeriodUs - kOnUs[s_intensity];
    s_pwm_due = esp_timer_get_time() + delay;
    ArmTimer(s_pwm_timer, delay);
}

esp_err_t Vibration_Init(const VibrationConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config.feedback_pulse_ms[0] <= 0 || config.feedback_pulse_ms[1] <= 0 || config.feedback_pulse_ms[2] <= 0 ||
        config.pulse_ms <= 0 || (config.drive != VibrationDrive::Gpio && config.drive != VibrationDrive::Pwm) ||
        (config.output != VibrationOutput::DirectPin && config.output != VibrationOutput::ExpandedPin) ||
        (config.output == VibrationOutput::DirectPin && !GPIO_IS_VALID_OUTPUT_GPIO(config.gpio)) ||
        (config.output == VibrationOutput::ExpandedPin && !config.write_expanded)) {
        ESP_LOGE(TAG, "invalid output configuration");
        return ESP_ERR_INVALID_ARG;
    }
    s_config = config;
    if (config.output == VibrationOutput::DirectPin) {
        gpio_config_t gpio{};
        gpio.pin_bit_mask = 1ULL << config.gpio;
        gpio.mode = GPIO_MODE_OUTPUT;
        const esp_err_t err = gpio_config(&gpio);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO initialization failed code=%d", err);
            return err;
        }
        gpio_sleep_sel_dis(config.gpio);
    }
    const esp_err_t output_err = WriteOutput(false);
    if (output_err != ESP_OK) {
        return output_err;
    }
    esp_timer_create_args_t args{};
    args.callback = PulseEnd;
    args.name = "vibe_pulse";
    args.dispatch_method = ESP_TIMER_TASK;
    args.skip_unhandled_events = true;
    esp_err_t err = esp_timer_create(&args, &s_pulse_timer);
    if (err == ESP_OK && config.drive == VibrationDrive::Pwm) {
        args.callback = PwmTick;
        args.name = "vibe_pwm";
        err = esp_timer_create(&args, &s_pwm_timer);
    }
    if (err != ESP_OK) {
        if (s_pulse_timer) {
            esp_timer_delete(s_pulse_timer);
        }
        s_pulse_timer = nullptr;
        ESP_LOGE(TAG, "timer creation failed code=%d", err);
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "ready drive=%d output=%d", static_cast<int>(config.drive), static_cast<int>(config.output));
    return ESP_OK;
}

uint32_t Vibration_PulseFor(int duration_ms) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || s_continuous || s_intensity == 0) {
        return 0;
    }
    if (duration_ms <= 0) {
        duration_ms = s_config.pulse_ms;
    }
    s_pulse_until = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    if (++s_pulse_id == 0) {
        ++s_pulse_id;
    }
    ApplyDrive();
    if (s_pulse_until) {
        ArmTimer(s_pulse_timer, static_cast<uint64_t>(duration_ms) * 1000);
    }
    return s_pulse_until ? s_pulse_id : 0;
}

void Vibration_CancelPulse(uint32_t pulse_id) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (pulse_id && pulse_id == s_pulse_id && s_pulse_until && !s_continuous) {
        Vibration_Set(false);
    }
}

bool Vibration_PulseActive(void) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_pulse_until != 0;
}

void Vibration_Set(bool on) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) {
        return;
    }
    esp_timer_stop(s_pulse_timer);
    s_pulse_until = 0;
    s_continuous = on && s_intensity != 0;
    if (s_continuous) {
        ApplyDrive();
    } else
        StopOutput();
}

void Vibration_SetIntensity(uint8_t level) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_intensity = level > 3 ? 3 : level;
    if (!s_initialized) {
        return;
    }
    if (s_continuous || s_pulse_until) {
        ApplyDrive();
    } else if (!s_intensity) {
        StopOutput();
    }
}

uint8_t Vibration_GetIntensity() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_intensity;
}

void Vibration_PulseFeedback(uint8_t level) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) {
        return;
    }
    Vibration_PulseFor(s_config.feedback_pulse_ms[level <= 2 ? level : 0]);
}
