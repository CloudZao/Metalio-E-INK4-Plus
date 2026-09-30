#pragma once

#include <cstdint>

#include <driver/gpio.h>
#include <esp_timer.h>

/**
 * @brief 前光驱动方式
 */
enum class FrontlightDriveMode {
    kPwm = 0, // LEDC PWM 调光（默认）
    kGpio,    // GPIO 高低电平开关（亮度>0 为开）
};

/**
 * @brief 前光色温通道组合
 */
enum class FrontlightCct {
    kWarm = 0, // 仅暖
    kCool,     // 仅冷
    kBoth,     // 冷暖同开
    kOff,      // 全关
};

/**
 * @brief 双路前光（冷/暖）单例：PWM 或 GPIO 驱动，亮度 0–100 梯度变化
 */
class Frontlight {
public:
    /**
     * @brief 获取单例
     */
    static Frontlight& GetInstance();

    /**
     * @brief 初始化硬件，默认 PWM、关闭、亮度 0
     * @param cool_pin 冷光 EN/PWM 脚
     * @param warm_pin 暖光 EN/PWM 脚
     * @param freq_hz PWM 频率 Hz，仅 PWM 模式使用
     * @return true 成功；已初始化则直接返回 true
     */
    bool Init(gpio_num_t cool_pin, gpio_num_t warm_pin, uint32_t freq_hz = 20000);

    /**
     * @brief 关闭输出并释放 GPIO/LEDC，可再次 Init
     */
    void Deinit();

    /**
     * @brief 切换 PWM / GPIO 驱动；保持当前亮度与色温
     * @param mode 目标驱动方式
     */
    void SetDriveMode(FrontlightDriveMode mode);

    /**
     * @brief 当前驱动方式
     */
    FrontlightDriveMode drive_mode() const { return drive_mode_; }

    /**
     * @brief 从 NVS 恢复亮度（默认 40，范围 0–100）；瞬时到位，不走渐变
     */
    void RestoreBrightness();

    /**
     * @brief 设置目标亮度（0–100），经定时器逐步过渡
     * @param brightness 目标亮度
     * @param permanent true 时写入 NVS
     */
    void SetBrightness(uint8_t brightness, bool permanent = false);

    /**
     * @brief 当前亮度（渐变过程中的瞬时值）
     */
    uint8_t brightness() const { return brightness_; }

    /**
     * @brief 目标亮度
     */
    uint8_t target_brightness() const { return target_brightness_; }

    /**
     * @brief 设置色温通道组合，立即按当前亮度输出
     * @param cct 暖 / 冷 / 同开 / 关
     */
    void SetCct(FrontlightCct cct);

    /**
     * @brief 暖 → 冷 → 同开 → 关 → 暖
     */
    void ToggleCct();

    /**
     * @brief 当前色温组合
     */
    FrontlightCct cct() const { return cct_; }

    /**
     * @brief 立即关输出（停渐变、亮度置 0），保留 PWM/GPIO 驱动挂接，供待机/浅睡关光
     */
    void ForceAllLow();

    /**
     * @brief 是否已初始化
     */
    bool initialized() const { return initialized_; }

private:
    Frontlight();
    ~Frontlight();
    Frontlight(const Frontlight&) = delete;
    Frontlight& operator=(const Frontlight&) = delete;

    void OnTransitionTimer();
    void ApplyOutput(uint8_t brightness);
    bool SetupHardware();
    void TeardownHardware();

    esp_timer_handle_t transition_timer_ = nullptr; // 亮度梯度定时器
    gpio_num_t cool_pin_ = GPIO_NUM_NC;             // 冷光脚
    gpio_num_t warm_pin_ = GPIO_NUM_NC;             // 暖光脚
    uint32_t freq_hz_ = 20000;                      // PWM 频率
    FrontlightDriveMode drive_mode_ = FrontlightDriveMode::kPwm; // 驱动方式
    FrontlightCct cct_ = FrontlightCct::kOff;       // 色温组合（默认关）
    uint8_t brightness_ = 0;                        // 当前亮度
    uint8_t target_brightness_ = 0;                 // 目标亮度
    int8_t step_ = 1;                               // 每拍步进
    bool initialized_ = false;                      // 已初始化
};
