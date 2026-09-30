/**
 * @file frontlight.cc
 * @brief 前光单例：双路冷/暖，PWM 或 GPIO 驱动，亮度梯度
 */

#include "frontlight.h"

#include "settings.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>

#define TAG "frontlight"

#define FL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define FL_LEDC_TIMER   LEDC_TIMER_0
#define FL_CH_COOL      LEDC_CHANNEL_0
#define FL_CH_WARM      LEDC_CHANNEL_1
#define FL_LEDC_RES     LEDC_TIMER_10_BIT
#define FL_DUTY_MAX     1023
#define FL_STEP_US      (5 * 1000) // 与参考 Backlight 一致：每 5ms 步进 1

Frontlight& Frontlight::GetInstance() {
    static Frontlight instance;
    return instance;
}

Frontlight::Frontlight() {
    const esp_timer_create_args_t args = {
        .callback =
            [](void* arg) {
                static_cast<Frontlight*>(arg)->OnTransitionTimer();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "frontlight_tr",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &transition_timer_));
}

Frontlight::~Frontlight() {
    Deinit();
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
        transition_timer_ = nullptr;
    }
}

bool Frontlight::Init(gpio_num_t cool_pin, gpio_num_t warm_pin, uint32_t freq_hz) {
    if (initialized_) {
        return true;
    }
    if (cool_pin == GPIO_NUM_NC || warm_pin == GPIO_NUM_NC || cool_pin == warm_pin) {
        ESP_LOGE(TAG, "invalid pins cool=%d warm=%d", (int)cool_pin, (int)warm_pin);
        return false;
    }
    if (freq_hz == 0) {
        ESP_LOGE(TAG, "freq_hz=0");
        return false;
    }

    cool_pin_ = cool_pin;
    warm_pin_ = warm_pin;
    freq_hz_ = freq_hz;
    drive_mode_ = FrontlightDriveMode::kPwm;
    cct_ = FrontlightCct::kOff;
    brightness_ = 0;
    target_brightness_ = 0;

    if (!SetupHardware()) {
        return false;
    }
    initialized_ = true;
    ApplyOutput(0);
    ESP_LOGI(TAG, "init ok mode=pwm cool=%d warm=%d freq=%u", (int)cool_pin_, (int)warm_pin_,
             (unsigned)freq_hz_);
    return true;
}

void Frontlight::Deinit() {
    if (!initialized_) {
        return;
    }
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
    }
    brightness_ = 0;
    target_brightness_ = 0;
    ApplyOutput(0);
    TeardownHardware();
    initialized_ = false;
    ESP_LOGI(TAG, "deinit");
}

void Frontlight::SetDriveMode(FrontlightDriveMode mode) {
    if (!initialized_ || mode == drive_mode_) {
        return;
    }
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
    }
    TeardownHardware();
    drive_mode_ = mode;
    if (!SetupHardware()) {
        ESP_LOGE(TAG, "SetDriveMode setup fail");
        initialized_ = false;
        return;
    }
    ApplyOutput(brightness_);
    ESP_LOGI(TAG, "drive=%s brightness=%u cct=%d",
             mode == FrontlightDriveMode::kPwm ? "pwm" : "gpio", (unsigned)brightness_,
             (int)cct_);
}

void Frontlight::RestoreBrightness() {
    if (!initialized_) {
        return;
    }
    Settings settings("frontlight");
    int saved_brightness = settings.GetInt("brightness", 40);
    if (saved_brightness < 0) {
        saved_brightness = 0;
    } else if (saved_brightness > 100) {
        saved_brightness = 100;
    }
    // 开机恢复瞬时到位，避免首帧已出仍在 5ms/档爬升
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
    }
    brightness_ = static_cast<uint8_t>(saved_brightness);
    target_brightness_ = brightness_;
    ApplyOutput(brightness_);
    ESP_LOGI(TAG, "Restore brightness to %d", brightness_);
}

void Frontlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (!initialized_) {
        return;
    }
    if (brightness > 100) {
        brightness = 100;
    }

    if (brightness_ == brightness) {
        return;
    }

    if (permanent) {
        Settings settings("frontlight", true);
        settings.SetInt("brightness", brightness);
    }

    target_brightness_ = brightness;
    step_ = (target_brightness_ > brightness_) ? 1 : -1;
    if (transition_timer_ != nullptr) {
        esp_timer_start_periodic(transition_timer_, FL_STEP_US);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Frontlight::SetCct(FrontlightCct cct) {
    if (!initialized_) {
        return;
    }
    cct_ = cct;
    ApplyOutput(brightness_);
}

void Frontlight::ToggleCct() {
    if (!initialized_) {
        return;
    }
    cct_ = static_cast<FrontlightCct>((static_cast<int>(cct_) + 1) % 4);
    ApplyOutput(brightness_);
    static const char* names[] = {"warm", "cool", "both", "off"};
    ESP_LOGI(TAG, "cct -> %s", names[static_cast<int>(cct_)]);
}

void Frontlight::ForceAllLow() {
    if (!initialized_) {
        return;
    }
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
    }
    // 只把占空比/电平拉到 0，不拆 LEDC 矩阵；否则退出待机 Restore 无法再亮
    brightness_ = 0;
    target_brightness_ = 0;
    ApplyOutput(0);
    ESP_LOGI(TAG, "force all low");
}

void Frontlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }
    brightness_ = static_cast<uint8_t>(static_cast<int>(brightness_) + step_);
    ApplyOutput(brightness_);
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
}

bool Frontlight::SetupHardware() {
    if (drive_mode_ == FrontlightDriveMode::kPwm) {
        const ledc_timer_config_t timer = {
            .speed_mode = FL_LEDC_MODE,
            .duty_resolution = FL_LEDC_RES,
            .timer_num = FL_LEDC_TIMER,
            .freq_hz = freq_hz_,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&timer) != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config fail");
            return false;
        }

        ledc_channel_config_t ch = {
            .gpio_num = cool_pin_,
            .speed_mode = FL_LEDC_MODE,
            .channel = FL_CH_COOL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = FL_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .flags = {.output_invert = 0},
        };
        if (ledc_channel_config(&ch) != ESP_OK) {
            ESP_LOGE(TAG, "ledc cool channel fail");
            return false;
        }
        ch.channel = FL_CH_WARM;
        ch.gpio_num = warm_pin_;
        if (ledc_channel_config(&ch) != ESP_OK) {
            ESP_LOGE(TAG, "ledc warm channel fail");
            return false;
        }
        return true;
    }

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << cool_pin_) | (1ULL << warm_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fail");
        return false;
    }
    gpio_set_level(cool_pin_, 0);
    gpio_set_level(warm_pin_, 0);
    return true;
}

void Frontlight::TeardownHardware() {
    if (cool_pin_ == GPIO_NUM_NC || warm_pin_ == GPIO_NUM_NC) {
        return;
    }
    if (drive_mode_ == FrontlightDriveMode::kPwm) {
        ledc_stop(FL_LEDC_MODE, FL_CH_COOL, 0);
        ledc_stop(FL_LEDC_MODE, FL_CH_WARM, 0);
    }
    gpio_reset_pin(cool_pin_);
    gpio_reset_pin(warm_pin_);
}

void Frontlight::ApplyOutput(uint8_t brightness) {
    const bool cool_on = (cct_ == FrontlightCct::kCool || cct_ == FrontlightCct::kBoth);
    const bool warm_on = (cct_ == FrontlightCct::kWarm || cct_ == FrontlightCct::kBoth);

    if (drive_mode_ == FrontlightDriveMode::kPwm) {
        const uint32_t duty = (FL_DUTY_MAX * brightness) / 100;
        ledc_set_duty(FL_LEDC_MODE, FL_CH_COOL, cool_on ? duty : 0);
        ledc_update_duty(FL_LEDC_MODE, FL_CH_COOL);
        ledc_set_duty(FL_LEDC_MODE, FL_CH_WARM, warm_on ? duty : 0);
        ledc_update_duty(FL_LEDC_MODE, FL_CH_WARM);
        return;
    }

    const int level = brightness > 0 ? 1 : 0;
    gpio_set_level(cool_pin_, cool_on ? level : 0);
    gpio_set_level(warm_pin_, warm_on ? level : 0);
}
