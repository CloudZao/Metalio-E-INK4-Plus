#include <atomic>
#include <new>
#include <driver/gpio.h>
#include <esp_log.h>
#include "gpio_button_input.h"
#include "boards/common/input/input.h"
#include "boards/common/io/IOExpander.hpp"
#include "config.h"

static constexpr const char* TAG = "GpioButtonInput";
static std::atomic<bool> s_power_click{false};
static std::atomic<bool> s_shutdown{false};
bool InputTakePowerClick() {
    return s_power_click.exchange(false);
}
bool InputTakeShutdownRequest() {
    return s_shutdown.exchange(false);
}

// TCA9555 按键：iot_button 自定义驱动轮询 IOExpander（默认低有效）
struct ExpanderButtonDriver {
    button_driver_t base{};                            // iot_button 驱动基座
    IOExpander::Pin pin = IOExpander::Pin::kPinCount;  // 扩展器引脚
    InputKey key = InputKeyVolumeUp;                   // 失败时取消的逻辑按键
    bool read_failed = false;                          // 异常只交付一次取消事件
    uint8_t active_level = 0;                          // 按下有效电平
};

static uint8_t ExpanderButton_GetKeyLevel(button_driver_t* button_driver) {
    auto* self = reinterpret_cast<ExpanderButtonDriver*>(button_driver);
    uint8_t level = 1;
    if (IOExpander::getInstance().getLevel(self->pin, &level) != ESP_OK) {
        if (!self->read_failed) {
            InputPostEvent(self->key, InputCancel);
        }
        self->read_failed = true;
        return 0;
    }
    self->read_failed = false;
    return level == self->active_level ? 1 : 0;
}

static esp_err_t ExpanderButton_Delete(button_driver_t* button_driver) {
    delete reinterpret_cast<ExpanderButtonDriver*>(button_driver);
    return ESP_OK;
}

static button_handle_t ExpanderButton_Create(IOExpander::Pin pin, InputKey key, bool active_high = false) {
    auto* drv = new (std::nothrow) ExpanderButtonDriver();
    if (!drv) {
        ESP_LOGE(TAG, "operation=create_key stage=driver result=no_memory");
        return nullptr;
    }
    drv->pin = pin;
    drv->key = key;
    drv->active_level = active_high ? 1 : 0;
    drv->base.enable_power_save = false;
    drv->base.get_key_level = ExpanderButton_GetKeyLevel;
    drv->base.enter_power_save = nullptr;
    drv->base.del = ExpanderButton_Delete;

    button_config_t cfg = {};
    cfg.long_press_time = kBootLongPressMs;
    cfg.short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS;
    button_handle_t handle = nullptr;
    const auto result = iot_button_create(&cfg, &drv->base, &handle);
    if (result != ESP_OK) {
        delete drv;
        ESP_LOGE(TAG, "operation=create_key stage=button code=%d", static_cast<int>(result));
        return nullptr;
    }
    return handle;
}

static void BindGestures(Button& button, InputKey key) {
    button.OnPressDown([key]() { InputPostEvent(key, InputPressDown); });
    button.OnPressUp([key]() { InputPostEvent(key, InputPressUp); });
    button.OnClick([key]() { InputPostEvent(key, InputClick); });
    button.OnDoubleClick([key]() { InputPostEvent(key, InputDoubleClick); });
    button.OnLongPress([key]() { InputPostEvent(key, InputLongPress); });
}

static std::unique_ptr<Button> CreateVolumeButton(IOExpander::Pin pin, InputKey key) {
    auto handle = ExpanderButton_Create(pin, key);
    if (!handle)
        return {};
    std::unique_ptr<Button> button(new (std::nothrow) Button(handle));
    if (!button) {
        iot_button_delete(handle);
        ESP_LOGE(TAG, "operation=create_key stage=wrapper result=no_memory");
        return {};
    }
    BindGestures(*button, key);
    return button;
}

void GpioButtonInput_Init(Button& boot, Button& power, std::unique_ptr<Button>& volume_up,
                          std::unique_ptr<Button>& volume_down, PowerKeyGestureState& power_key) {
    power_key.suppress = gpio_get_level(POWER_BUTTON_GPIO) == 0;
    power_key.swallow_click = false;
    BindGestures(boot, InputKeyBoot);
    power.OnPressDown([&power_key]() {
        if (!power_key.suppress) {
            InputPostEvent(InputKeyPower, InputPressDown);
        }
    });
    power.OnPressUp([&power_key]() {
        InputPostEvent(InputKeyPower, InputPressUp);
        if (power_key.suppress) {
            power_key.suppress = false;
            power_key.swallow_click = true;
        }
    });
    power.OnClick([&power_key]() {
        if (power_key.suppress) {
            return;
        }
        if (power_key.swallow_click) {
            power_key.swallow_click = false;
            return;
        }
        s_power_click.store(true);
    });
    power.OnLongPress([&power_key]() {
        if (!power_key.suppress) {
            s_shutdown.store(true);
        }
    });
    if (!IOExpander::getInstance().isInitialized()) {
        ESP_LOGW(TAG, "operation=init stage=volume_keys result=io_unavailable");
        return;
    }
    volume_down = CreateVolumeButton(IOExpander::Pin::VOLUME_DOWN, InputKeyVolumeDown);
    volume_up = CreateVolumeButton(IOExpander::Pin::VOLUME_UP, InputKeyVolumeUp);
    ESP_LOGI(TAG, "input sampling ready");
}

bool InputKeyIsDown(InputKey key) {
    if (key == InputKeyBoot || key == InputKeyPower)
        return gpio_get_level(key == InputKeyBoot ? BOOT_BUTTON_GPIO : POWER_BUTTON_GPIO) == 0;
    if (key != InputKeyVolumeUp && key != InputKeyVolumeDown) {
        return false;
    }
    uint8_t level = 1;
    const auto pin = key == InputKeyVolumeUp ? IOExpander::Pin::VOLUME_UP : IOExpander::Pin::VOLUME_DOWN;
    return IOExpander::getInstance().getLevel(pin, &level) == ESP_OK && level == 0;
}
