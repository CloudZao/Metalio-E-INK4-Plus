#pragma once

#include <stdint.h>
#include <driver/gpio.h>
#include <esp_err.h>

enum class VibrationDrive { Gpio, Pwm };                // 开关输出或低频软件 PWM
enum class VibrationOutput { DirectPin, ExpandedPin };  // MCU 引脚或扩展引脚

struct VibrationConfig {
    VibrationDrive drive;                         // 驱动方式
    VibrationOutput output;                       // 输出连接方式
    gpio_num_t gpio = GPIO_NUM_NC;                // 直连引脚，仅 DirectPin 使用
    bool active_high = true;                      // 直连引脚有效电平
    esp_err_t (*write_expanded)(bool) = nullptr;  // 扩展引脚逻辑输出，板级保证器件已就绪
    int feedback_pulse_ms[3] = {};                // 板级提供弱、中、强反馈脉宽，须为正值，ms
    int pulse_ms = 40;                            // 默认脉冲时长
};

/** @brief 初始化振动输出及必要定时器，启动时调用一次 */
esp_err_t Vibration_Init(const VibrationConfig& config);

/** @brief 请求短振动；返回非零脉冲标识，未启动返回零，非正时长使用板级默认值 */
uint32_t Vibration_PulseFor(int duration_ms);

/** @brief 仅取消标识匹配的在途脉冲，不影响后续反馈或连续振动 */
void Vibration_CancelPulse(uint32_t pulse_id);

/** @brief 限时脉冲是否仍在输出，不含连续振动 */
bool Vibration_PulseActive(void);

/** @brief 开启或关闭连续振动 */
void Vibration_Set(bool on);

/** @brief 设置强度零至三，GPIO 开关模式下非零强度均为全电平 */
void Vibration_SetIntensity(uint8_t level);

/** @brief 获取当前强度档位 */
uint8_t Vibration_GetIntensity();

/** @brief 按零至二档请求反馈，脉宽来自板级配置 */
void Vibration_PulseFeedback(uint8_t level);
