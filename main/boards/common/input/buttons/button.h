#ifndef BUTTON_H_
#define BUTTON_H_

#include <functional>
#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>

class Button {
public:
    /** @brief 接管已有按键句柄，析构时释放对应按键 */
    Button(button_handle_t button_handle);

    /** @brief 创建 GPIO 按键；无效引脚不创建器件，时长参数单位为毫秒 */
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0,
           bool enable_power_save = false);

    /** @brief 注销按键回调并释放按键句柄 */
    ~Button();

    /** @brief 设置按下事件回调，由按键驱动任务调用 */
    void OnPressDown(std::function<void()> callback);

    /** @brief 设置松开事件回调，由按键驱动任务调用 */
    void OnPressUp(std::function<void()> callback);

    /** @brief 设置达到长按阈值时的事件回调 */
    void OnLongPress(std::function<void()> callback);

    /** @brief 设置单击事件回调 */
    void OnClick(std::function<void()> callback);

    /** @brief 设置双击事件回调 */
    void OnDoubleClick(std::function<void()> callback);

    /** @brief 设置指定连续点击次数的事件回调 */
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);

protected:
    gpio_num_t gpio_num_;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    /** @brief 按 ADC 配置创建模拟按键，复用基类事件注册接口 */
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

class PowerSaveButton : public Button {
public:
    /** @brief 创建支持省电采样的低电平有效 GPIO 按键 */
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {}
};

#endif  // BUTTON_H_
