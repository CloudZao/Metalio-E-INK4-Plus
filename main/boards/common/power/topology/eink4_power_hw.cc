#include <stdio.h>
#include <atomic>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_button.h>
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "boards/common/io/IOExpander.hpp"
#include "boards/common/power/cx25601n/cx25601n.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "boards/common/storage/sd_access.h"
#include "config.h"
#include "infra/storage/key_bindings.h"

static const char* TAG = "PowerHw";

static bool s_main_rail_known = false;
static bool s_main_rail_on = false;
static std::atomic<uint32_t> s_key_wake_latch{0};
static bool s_gpio_wake_boot_low = false;
static bool s_gpio_wake_power_low = false;

// 浅睡时 SPI 外设掉电，EPD_RST 易漂；钉住高电平，避免误复位导致墨水漂移
static void EpdRstHoldForLightSleep(void) {
    gpio_set_level(EPD_PIN_RST, 1);
    esp_err_t err = gpio_hold_en(EPD_PIN_RST);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_hold_en(RST): %s", esp_err_to_name(err));
    }
}

static void EpdRstReleaseAfterLightSleep(void) {
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_hold_dis(EPD_PIN_RST);
}

esp_err_t power_hw_pa_set(bool on) {
    static bool known = false;
    static bool last_on = false;
    if (known && last_on == on) {
        return ESP_OK;
    }

    auto& io = IOExpander::getInstance();
    if (!io.isInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = io.setLevel(IOExpander::Pin::PA, on);
    if (err == ESP_OK) {
        known = true;
        last_on = on;
        ESP_LOGI(TAG, "PA %s", on ? "on" : "off");
    }
    return err;
}

bool power_hw_main_rail_is_on(void) {
    return s_main_rail_known && s_main_rail_on;
}

bool power_hw_main_rail_is_known(void) {
    return s_main_rail_known;
}

static esp_err_t MainRailSetImpl(bool on, bool reinit_peripherals_on_rise) {
    if (s_main_rail_known && s_main_rail_on == on) {
        return ESP_OK;
    }

    auto& io = IOExpander::getInstance();
    if (!io.isInitialized()) {
        ESP_LOGW(TAG, "MAIN_PWR %s skipped (IO not ready)", on ? "on" : "off");
        return ESP_ERR_INVALID_STATE;
    }

    // 仅「已知曾掉电后再上电」才重初始化外设；开机首次对齐 known 不重复发 AT
    const bool rising = on && s_main_rail_known && !s_main_rail_on;
    const bool falling = !on && (!s_main_rail_known || s_main_rail_on);

    // 掉电前先停 4G（供电仍在），再拉低 MAIN_PWR；WiFi 模式 no-op
    if (falling) {
        BluetoothAudio_OnPowerOff();
        power_hw_network_prepare_off();
    }

    esp_err_t err = io.setLevel(IOExpander::Pin::MAIN_PWR, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAIN_PWR %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
        // 已 TearDown 但 GPIO 掉电失败：轨仍上电、软件无 modem → 必须重建，否则永久失联
        if (falling) {
            BluetoothAudio_OnPowerRestored();
            BluetoothAudio_RequestModeInit();
            ESP_LOGW(TAG, "MAIN_PWR off failed after cellular teardown; re-init");
            power_hw_network_restored(true);
        }
        return err;
    }

    s_main_rail_known = true;
    s_main_rail_on = on;
    ESP_LOGD(TAG, "MAIN_PWR %s", on ? "on" : "off");

    // 掉电后模组状态丢失；上升沿异步恢复 BT 模式1 + 4G Start（不阻塞 Flush/定时器）
    // reinit_peripherals_on_rise=false：关机 hold 上电，勿重拉模组
    if (rising && reinit_peripherals_on_rise) {
        BluetoothAudio_OnPowerRestored();
        BluetoothAudio_RequestModeInit();
        power_hw_network_restored(true);
    }
    return ESP_OK;
}

esp_err_t power_hw_main_rail_set(bool on) {
    // 对齐 470 的共享供电策略：空闲和待机请求不切断蓝牙与蜂窝的公共电源。
    return on ? MainRailSetImpl(true, true) : ESP_OK;
}

esp_err_t power_hw_prepare_power_off() {
    auto& io = IOExpander::getInstance();
    if (!io.isInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    power_hw_pa_set(false);
    // 关机刷图前须保证外设轨在位；同步 known 态，但不重发 BT（即将断电）
    esp_err_t err_main = MainRailSetImpl(true, false);
    esp_err_t err_sock = io.setLevel(IOExpander::Pin::SCREEN_SOCKET_PWR, true);
    if (err_sock != ESP_OK || err_main != ESP_OK) {
        ESP_LOGW(TAG, "power-off prep rails: sock=%s main=%s", esp_err_to_name(err_sock), esp_err_to_name(err_main));
        return (err_sock != ESP_OK) ? err_sock : err_main;
    }
    ESP_LOGD(TAG, "power-off prep: PA off, MAIN/SCREEN rails held");
    return ESP_OK;
}

esp_err_t power_hw_power_off_pulse() {
    auto& io = IOExpander::getInstance();
    const esp_err_t high = io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    const esp_err_t low = io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
    return high != ESP_OK ? high : low;
}

esp_err_t power_hw_light_sleep_once(uint64_t max_sleep_us) {
    if (power_hw_sleep_cancelled() || !SdAccessIsQuiescing() || !SdAccessIsIdle()) {
        return ESP_ERR_INVALID_STATE;
    }
    #if CONFIG_PM_ENABLE
    const esp_err_t buttons = iot_button_stop();
    if (buttons != ESP_OK) {
        ESP_LOGW(TAG, "sleep button pause failed: code=%d", int(buttons));
        return buttons;
    }

    struct ButtonResume {
        ~ButtonResume() {
            const auto result = iot_button_resume();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "sleep button resume failed: code=%d", int(result));
            }
        }
    } button_resume;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(BOOT_BUTTON_GPIO);
    gpio_wakeup_disable(POWER_BUTTON_GPIO);
    gpio_wakeup_disable(IO_EXPANDER_INT_GPIO);
    esp_err_t err = ESP_OK;
    if (KeyBindingsNeedsWake(BindingKey::Boot)) err = gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK && KeyBindingsNeedsWake(BindingKey::Power)) err = gpio_wakeup_enable(POWER_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK && (KeyBindingsNeedsWake(BindingKey::VolumeUp) || KeyBindingsNeedsWake(BindingKey::VolumeDown))) {
        if (Accelerometer_Ok()) {
            Accelerometer_ReleaseIntLine();
        }
        vTaskDelay(pdMS_TO_TICKS(21));
        uint8_t idle = 1;
        auto& io = IOExpander::getInstance();
        err = io.getLevel(IOExpander::Pin::VOLUME_UP, &idle);
        if (err == ESP_OK) err = io.getLevel(IOExpander::Pin::VOLUME_DOWN, &idle);
        if (err == ESP_OK) err = gpio_wakeup_enable(IO_EXPANDER_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "operation=arm_keys code=%d", int(err));
        return err;
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio wake enable failed: %s", esp_err_to_name(err));
        return err;
    }
    if (max_sleep_us > 0) {
        err = esp_sleep_enable_timer_wakeup(max_sleep_us);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "timer wake cfg failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGD(TAG, "blocking light sleep; POWER GPIO%d%s", static_cast<int>(POWER_BUTTON_GPIO),
             max_sleep_us > 0 ? " + timer" : "");
    // 浅睡后 UART 停：尽量刷出「即将入睡」日志
    fflush(stdout);
    #if defined(CONFIG_ESP_CONSOLE_UART_NUM) && (CONFIG_ESP_CONSOLE_UART_NUM >= 0)

    uart_wait_tx_idle_polling(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM));
    #endif
    if (!BlePageTurnerPrepareSleep()) {
        return ESP_ERR_INVALID_STATE;
    }
    EpdRstHoldForLightSleep();

    cx25601n_vreg_suspend_for_sleep();
    err = esp_light_sleep_start();
    cx25601n_vreg_resume_after_sleep();

    // 醒瞬间先锁存 POWER 电平，再打日志（UART 刷写会拖过短按）
    s_gpio_wake_boot_low = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    s_gpio_wake_power_low = gpio_get_level(POWER_BUTTON_GPIO) == 0;
    uint32_t latch = (s_gpio_wake_boot_low ? 1u : 0u) | (s_gpio_wake_power_low ? 2u : 0u);
    uint8_t level = 1;
    auto& io = IOExpander::getInstance();
    if (io.getLevel(IOExpander::Pin::VOLUME_UP, &level) == ESP_OK && level == 0) latch |= 1u << 2;
    if (io.getLevel(IOExpander::Pin::VOLUME_DOWN, &level) == ESP_OK && level == 0) latch |= 1u << 3;
    s_key_wake_latch.store(latch);
    gpio_wakeup_disable(BOOT_BUTTON_GPIO);
    gpio_wakeup_disable(POWER_BUTTON_GPIO);
    gpio_wakeup_disable(IO_EXPANDER_INT_GPIO);
    EpdRstReleaseAfterLightSleep();
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGD(TAG, "woke from light sleep: %s cause=%d power=%d wake_us=%llu", esp_err_to_name(err), (int)cause,
             s_gpio_wake_power_low ? 1 : 0, static_cast<unsigned long long>(esp_timer_get_time()));
    return err;
    #else
    (void)max_sleep_us;
    s_gpio_wake_boot_low = false;
    s_gpio_wake_power_low = false;
    ESP_LOGW(TAG, "light sleep skipped (CONFIG_PM_ENABLE off)");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

void power_hw_gpio_wake_latch_get(bool* boot_low, bool* power_low) {
    if (boot_low != nullptr) {
        *boot_low = s_gpio_wake_boot_low;
    }
    if (power_low != nullptr) {
        *power_low = s_gpio_wake_power_low;
    }
}

esp_err_t power_hw_sleep_init(void) {
    return ESP_OK;
}

void power_hw_report_sleep_reset(void) {
    static bool reported = false;
    if (reported) {
        return;
    }
    reported = true;
    ESP_LOGI(TAG, "reset reason=%d", int(esp_reset_reason()));
}

static bool s_sd_parked = false;
static bool s_sd_restore_needed = false;

esp_err_t power_hw_sd_park(void) {
    if (s_sd_parked) {
        return ESP_OK;
    }
    if (!SdAccessIsIdle()) {
        return ESP_ERR_INVALID_STATE;
    }
    auto& sd = SdCardManager::GetInstance();
    s_sd_restore_needed = sd.IsMounted();
    if (s_sd_restore_needed && !sd.Unmount()) {
        ESP_LOGE(TAG, "standby SD unmount failed");
        return ESP_FAIL;
    }
    s_sd_parked = true;
    return ESP_OK;
}

esp_err_t power_hw_sd_restore(void) {
    if (!s_sd_parked) {
        return ESP_OK;
    }
    if (s_sd_restore_needed && !SdCardManager::GetInstance().Mount()) {
        ESP_LOGE(TAG, "standby SD remount failed");
        return ESP_FAIL;
    }
    s_sd_parked = false;
    return ESP_OK;
}

static bool s_standby_rails_prepared = false;

esp_err_t power_hw_standby_rails_park(void) {
    const esp_err_t err = power_hw_pa_set(false);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_standby_rails_prepared) {
        power_hw_network_prepare_off();
        s_standby_rails_prepared = true;
    }
    return ESP_OK;
}

esp_err_t power_hw_standby_rails_restore(void) {
    if (!s_standby_rails_prepared) {
        return ESP_OK;
    }
    const esp_err_t err = power_hw_main_rail_set(true);
    if (err == ESP_OK) {
        s_standby_rails_prepared = false;
    }
    return err;
}

void power_hw_boot_prepare(void) {
    power_hw_report_sleep_reset();
}

esp_err_t power_hw_bt_power_reset(void) {
    // 397没有独立蓝牙复位脚，共享电源轨不可用于单独复位。
    ESP_LOGW(TAG, "BT hardware reset unsupported on this board");
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t power_hw_take_key_wake_latch(void) {
    return s_key_wake_latch.exchange(0);
}
