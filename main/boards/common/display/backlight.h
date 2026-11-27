#pragma once

#include <cstdint>
#include <functional>
#include <driver/gpio.h>
#include <esp_timer.h>

class Backlight {
public:
    /** @brief 创建亮度渐变定时器，硬件输出由派生类实现 */
    Backlight();

    /** @brief 停止并释放亮度渐变定时器 */
    ~Backlight();

    /** @brief 读取已保存的亮度并启动渐变恢复，过低亮度使用默认值 */
    void RestoreBrightness();

    /** @brief 设置不超过百分之百的目标亮度，可选择保存偏好，输出逐步过渡 */
    void SetBrightness(uint8_t brightness, bool permanent = false);

    /** @brief 获取渐变过程中的当前亮度 */
    inline uint8_t brightness() const {
        return brightness_;
    }

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
};

class PwmBacklight : public Backlight {
public:
    /** @brief 配置指定 GPIO 的 PWM 背光输出，支持反相与自定义频率 */
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
