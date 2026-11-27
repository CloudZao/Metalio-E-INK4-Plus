/**
 * @brief S31板型的供电、引脚保持与唤醒时序
 */
#include <atomic>
#include <stdio.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/audio/bluetooth/simple_uart.hpp"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "boards/common/bus/flash_access.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"
#include "boards/common/display/frontlight.h"
#include "boards/common/haptics/vibration.h"
#include "boards/common/input/buttons/io_expander/expander_button_input.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/power/s31_sleep/s31_sleep.h"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "boards/common/storage/sd_access.h"
#include "boards/common/storage/sdmmc/sdmmc_storage.h"
#include "config.h"
#include "infra/storage/key_bindings.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include <hal/usb_serial_jtag_ll.h>

extern "C" bool usb_serial_jtag_is_connected(void);
#endif

#define TAG "PowerHw"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
static constexpr int kUsjReviveWaitConnectedMs = 800;
#endif

static std::atomic<int> s_main_rail_state{-1};  // -1=未知，0=关闭，1=开启；策略与联网任务共享
static bool s_sd_parked = false;
static bool s_standby_rails_prepared = false;  // 首次断电尝试后登记，退出待机或关机绘图前恢复
static std::atomic<uint32_t> s_key_wake_latch{0};
static bool s_gpio_wake_boot_low = false;
static bool s_gpio_wake_power_low = false;

// Keep board-facing entry points stable; SDK details live in s31_sleep.
void power_hw_boot_prepare(void) {
    S31SleepCaptureReset();
    epd_board_adapter_boot_release_holds();
}

void power_hw_report_sleep_reset(void) {
    S31SleepReportReset();
}

esp_err_t power_hw_sleep_init(void) {
    return S31SleepInit();
}

// 确认共享 INT 线为空闲高
static bool WaitIntLineHigh(void) {
    gpio_set_direction(IO_EXPANDER_INT_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(IO_EXPANDER_INT_GPIO);
    gpio_pulldown_dis(IO_EXPANDER_INT_GPIO);

    for (int i = 0; i < 30; ++i) {
        if (gpio_get_level(IO_EXPANDER_INT_GPIO) != 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "GPIO2 still low after mask");
    return false;
}

// EXT1：GPIO2 低电平唤醒（电源键 P17 → TCA INT）
static bool EnableGpio2Wake(void) {
    if (!esp_sleep_is_valid_wakeup_gpio(IO_EXPANDER_INT_GPIO)) {
        ESP_LOGE(TAG, "GPIO%d not valid for EXT1 wake", (int)IO_EXPANDER_INT_GPIO);
        return false;
    }
    const uint64_t wake_mask = 1ULL << static_cast<unsigned>(IO_EXPANDER_INT_GPIO);
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ext1 wake failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// 浅睡硬件停用范围；SD/CNNT 恢复后确认副控制台时钟与 PHY。
enum : unsigned {
    kLsSd = 1u << 0,     // HoldPinsLow + Latch；退出待机或关机绘图前 Sd_Init
    kLsTps = 1u << 1,    // tps_enter_sleep
    kLsBus = 1u << 2,    // bus_hold_low + latch；退出待机或关机绘图前 bus_restore
    kLsRails = 1u << 3,  // sleep_cut_rails；退出待机或关机绘图前 restore_rails
};

constexpr unsigned kLightSleepHw = kLsSd | kLsTps | kLsBus | kLsRails;

/**
 * @brief SD/CNNT 恢复后恢复副控制台时钟与 PHY
 * @note 在 SdmmcStorage_Init 之后调用，保持已有 USB 配置。
 */
static void ReviveUsjAfterSdRestore(void) {
    #if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    usb_serial_jtag_ll_enable_bus_clock(true);
    usb_serial_jtag_ll_phy_set_defaults();
    usb_serial_jtag_ll_phy_enable_pad(true);

    int waited = 0;
    while (waited < kUsjReviveWaitConnectedMs && !usb_serial_jtag_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    esp_rom_printf("PowerHw: usj revive after SD conn=%d wait=%d\n", usb_serial_jtag_is_connected() ? 1 : 0, waited);
    ESP_LOGI(TAG, "usj revive after SD conn=%d waited=%dms", usb_serial_jtag_is_connected() ? 1 : 0, waited);
    fflush(stdout);
    #endif
}

esp_err_t power_hw_sd_park(void) {
    SdAccessTransition transition;
    if (!transition) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_sd_parked) {
        return ESP_OK;
    }
    if (!SdAccessIsQuiescing() || !SdAccessIsIdle()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!SdmmcStorage_HoldPinsLow()) {
        return ESP_FAIL;
    }
    SdmmcStorage_LatchForSleep();
    s_sd_parked = true;
    return ESP_OK;
}

esp_err_t power_hw_sd_restore(void) {
    SdAccessTransition transition;
    if (!transition) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_sd_parked) {
        return ESP_OK;
    }
    const bool mounted = SdmmcStorage_Init();
    if (mounted) {
        s_sd_parked = false;
    }
    ReviveUsjAfterSdRestore();
    return mounted ? ESP_OK : ESP_FAIL;
}

static esp_err_t CutStandbyRails(void) {
    if (!s_standby_rails_prepared) {
        Frontlight::GetInstance().ForceAllLow();
        power_hw_network_prepare_off();
        // 写失败也可能已改变部分输出，首次尝试前即登记恢复责任。
        s_standby_rails_prepared = true;
    }
    return epd_board_adapter_sleep_cut_rails();
}

static esp_err_t RestoreStandbyRails(void) {
    if (!s_standby_rails_prepared) {
        return ESP_OK;
    }
    const esp_err_t err = epd_board_adapter_sleep_restore_rails();
    if (err == ESP_OK) {
        s_standby_rails_prepared = false;
        power_hw_network_restored(false);
    }
    Frontlight::GetInstance().ForceAllLow();
    return err;
}

esp_err_t power_hw_standby_rails_park(void) {
    if (!s_standby_rails_prepared) {
        const esp_err_t err = epd_board_adapter_tps_enter_sleep();
        if (err != ESP_OK) {
            return err;
        }
        epd_board_adapter_bus_hold_low();
    }
    return CutStandbyRails();
}

esp_err_t power_hw_standby_rails_restore(void) {
    const esp_err_t err = RestoreStandbyRails();
    if (err == ESP_OK) {
        epd_board_adapter_bus_restore();
    }
    return err;
}

static void SleepAbort(const char* why, unsigned applied) {
    gpio_wakeup_disable(BOOT_BUTTON_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGW(TAG, "%s (applied=0x%x)", why, applied);
    if ((applied & kLsRails) != 0) {
        const esp_err_t err = RestoreStandbyRails();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sleep abort: rail restore failed: %s", esp_err_to_name(err));
        }
    }
    if ((applied & kLsBus) != 0) {
        epd_board_adapter_bus_restore();
    }
    const esp_err_t keys_err = ExpanderButtonInput_ResumeAfterSleep();
    if (keys_err != ESP_OK) {
        ESP_LOGE(TAG, "sleep abort: key restore failed: %s", esp_err_to_name(keys_err));
    }
    if ((applied & kLsSd) != 0) {
        power_hw_sd_restore();
    }
}

esp_err_t power_hw_pa_set(bool on) {
    return BluetoothAudio_SetPa(on);
}

esp_err_t power_hw_bt_power_reset(void) {
    std::lock_guard<std::recursive_timed_mutex> transaction(SimpleUart::getInstance().commandMutex());
    power_hw_network_prepare_off();
    esp_err_t err;
    {
        // 等当前波形结束，并阻止 P14 断电期间探测/启动 TPS。
        // 4G 清理与恢复可能更新 UI，必须放在此锁之外。
        FlashCacheGuard panel_guard;
        Vibration_Set(false);
        BluetoothAudio_SetPa(false);
        BluetoothAudio_OnPowerOff();
        err = epd_board_adapter_bt_power_reset();
        if (err == ESP_OK) {
            // 与开机 P14 恢复路径一致，待电源稳定后才允许下一次刷屏。
            vTaskDelay(pdMS_TO_TICKS(POWER_RAIL_SETTLE_MS));
        }
    }
    if (err == ESP_OK) {
        BluetoothAudio_OnPowerRestored();
        power_hw_network_restored(true);
    }
    return err;
}

esp_err_t power_hw_main_rail_set(bool on) {
    // S31 无独立 MAIN_PWR；保持策略态幂等
    const int next = on ? 1 : 0;
    if (s_main_rail_state.exchange(next) == next) {
        return ESP_OK;
    }
    if (on) {
        power_hw_network_resume();
    }
    ESP_LOGD(TAG, "main_rail_set(%d) policy only", on ? 1 : 0);
    return ESP_OK;
}

bool power_hw_main_rail_is_on(void) {
    return s_main_rail_state.load() != 0;
}

bool power_hw_main_rail_is_known(void) {
    return s_main_rail_state.load() >= 0;
}

esp_err_t power_hw_prepare_power_off() {
    return power_hw_standby_rails_restore();
}

esp_err_t power_hw_power_off_pulse() {
    Frontlight::GetInstance().ForceAllLow();
    epd_board_adapter_pwr_key_pulse_train();
    return ESP_OK;
}

/** @brief 待机浅睡保留 SD/CNNT hold；成功返回仅恢复按键，外围在退出待机或关机绘图前恢复。 */
esp_err_t power_hw_light_sleep_once(uint64_t max_sleep_us) {
    // No timeout may force power removal with live files or boot hardware work.
    if (!SdAccessIsQuiescing() || !SdAccessIsIdle() || power_hw_sleep_cancelled()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!BlePageTurnerPrepareSleep()) {
        return ESP_ERR_INVALID_STATE;
    }
    SdAccessTransition transition;
    if (!transition) {
        return ESP_ERR_TIMEOUT;
    }
    unsigned applied = s_standby_rails_prepared ? kLsTps | kLsBus | kLsRails : 0;
    S31SleepBeginCycle();
    S31SleepCheckpoint(S31SleepPhase::Admission, applied);  // admission passed
    ESP_LOGI(TAG, "light sleep once max_us=%llu hw=0x%x (sd=%d tps=%d bus=%d rails=%d)",
             (unsigned long long)max_sleep_us, kLightSleepHw, (kLightSleepHw & kLsSd) ? 1 : 0,
             (kLightSleepHw & kLsTps) ? 1 : 0, (kLightSleepHw & kLsBus) ? 1 : 0, (kLightSleepHw & kLsRails) ? 1 : 0);

    if ((kLightSleepHw & kLsSd) != 0) {
        if (power_hw_sd_park() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        applied |= kLsSd;
    }
    if ((kLightSleepHw & kLsTps) != 0 && !s_standby_rails_prepared) {
        const esp_err_t err = epd_board_adapter_tps_enter_sleep();
        if (err != ESP_OK) {
            SleepAbort("abort light sleep: TPS sleep failed", applied);
            return err;
        }
        applied |= kLsTps;
    }
    if ((kLightSleepHw & kLsBus) != 0 && !s_standby_rails_prepared) {
        epd_board_adapter_bus_hold_low();
        applied |= kLsBus;
    }

    Accelerometer_ReleaseIntLine();
    S31SleepCheckpoint(S31SleepPhase::HardwareParked, applied);  // SD/TPS/bus parked
    const esp_err_t mask_err = ExpanderButtonInput_MaskForSleep();
    if (mask_err != ESP_OK) {
        SleepAbort("abort light sleep: key mask failed", applied);
        return mask_err;
    }
    if (ExpanderButtonInput_HasPowerClick() || power_hw_sleep_cancelled()) {
        SleepAbort("abort light sleep: pending POWER exit", applied);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t wake_inputs = 0;
    if (KeyBindingsNeedsWake(BindingKey::Power)) wake_inputs |= 1u << (EPD_IO_POWER & 7);
    if (KeyBindingsNeedsWake(BindingKey::VolumeUp)) wake_inputs |= 1u << (EPD_IO_VOL_UP & 7);
    if (KeyBindingsNeedsWake(BindingKey::VolumeDown)) wake_inputs |= 1u << (EPD_IO_VOL_DOWN & 7);
    if (!epd_board_adapter_ioexp_enable_key_wake_pins(wake_inputs)) {
        SleepAbort("abort light sleep: key inputs failed", applied);
        return ESP_ERR_INVALID_STATE;
    }
    if (!WaitIntLineHigh()) {
        SleepAbort("abort light sleep: GPIO2 low", applied);
        return ESP_ERR_INVALID_STATE;
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (!EnableGpio2Wake()) {
        SleepAbort("abort light sleep: wake cfg fail", applied);
        return ESP_ERR_INVALID_STATE;
    }
    if (KeyBindingsNeedsWake(BindingKey::Boot)) {
        esp_err_t boot_error = gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
        if (boot_error == ESP_OK) boot_error = esp_sleep_enable_gpio_wakeup();
        if (boot_error != ESP_OK || gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            SleepAbort("abort light sleep: BOOT wake", applied);
            gpio_wakeup_disable(BOOT_BUTTON_GPIO);
            return boot_error == ESP_OK ? ESP_ERR_INVALID_STATE : boot_error;
        }
    }
    if (max_sleep_us > 0) {
        esp_err_t terr = esp_sleep_enable_timer_wakeup(max_sleep_us);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "timer wake cfg failed: %s", esp_err_to_name(terr));
            SleepAbort("abort light sleep: timer", applied);
            return terr;
        }
    }

    if ((kLightSleepHw & kLsRails) != 0) {
        S31SleepCheckpoint(S31SleepPhase::WakeArmed, applied);  // wake sources armed; about to cut rails
        // 写失败也可能已改变部分输出，恢复责任在首次关轨尝试前登记。
        applied |= kLsRails;
        const esp_err_t err = CutStandbyRails();
        if (err != ESP_OK) {
            SleepAbort("abort light sleep: rail cut failed", applied);
            return err;
        }
    }
    if ((kLightSleepHw & kLsBus) != 0) {
        epd_board_adapter_bus_latch_for_sleep();
    }

    fflush(stdout);
    S31SleepCheckpoint(S31SleepPhase::BeforeSleep, applied);  // last checkpoint before the CPU sleeps
    esp_err_t err = S31SleepLightSleep(applied);
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // 醒瞬间锁存 P17；此时 USJ 可能仍哑，用 ROM 打一行便于对照
    s_gpio_wake_boot_low = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    s_gpio_wake_power_low = false;
    uint32_t key_latch = s_gpio_wake_boot_low ? 1u : 0u;
    uint8_t wake_port = 0xFF;
    if (epd_board_adapter_ioexp_read_port(1, &wake_port)) {
        s_gpio_wake_power_low = !(wake_port & (1u << (EPD_IO_POWER & 7)));
        if (s_gpio_wake_power_low) key_latch |= 1u << 1;
        if (!(wake_port & (1u << (EPD_IO_VOL_UP & 7)))) key_latch |= 1u << 2;
        if (!(wake_port & (1u << (EPD_IO_VOL_DOWN & 7)))) key_latch |= 1u << 3;
    }
    s_key_wake_latch.store(key_latch);
    gpio_wakeup_disable(BOOT_BUTTON_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_rom_printf("PowerHw: light_sleep wake err=%s cause=%d power_low=%d\n", esp_err_to_name(err), (int)cause,
                   s_gpio_wake_power_low ? 1 : 0);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    if (max_sleep_us > 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    const esp_err_t keys_err = ExpanderButtonInput_ResumeAfterSleep();
    if (keys_err == ESP_OK) {
        S31SleepCheckpoint(S31SleepPhase::KeysRestored, applied);
    } else {
        ESP_LOGE(TAG, "wake: key restore failed: %s", esp_err_to_name(keys_err));
        if (err == ESP_OK) {
            err = keys_err;
        }
    }
    if (err == ESP_OK) {
        // 定时及共享 INT 唤醒只恢复按键采样；确认退出或绘制关机画面时再恢复外围。
        return ESP_OK;
    }
    if ((applied & kLsRails) != 0) {
        const esp_err_t rail_err = RestoreStandbyRails();
        if (rail_err != ESP_OK) {
            ESP_LOGE(TAG, "wake: rail restore failed: %s", esp_err_to_name(rail_err));
        }
    }
    if ((applied & kLsSd) != 0) {
        S31SleepCheckpoint(S31SleepPhase::BeforeSdRestore,
                           applied);  // rails restored; about to restore SD/USB console
        power_hw_sd_restore();
    }
    if ((applied & kLsBus) != 0) {
        S31SleepCheckpoint(S31SleepPhase::BeforeBusRestore, applied);  // about to reinitialize display bus
        epd_board_adapter_bus_restore();
    }
    ESP_LOGI(TAG, "light_sleep wake err=%s cause=%d power_low=%d restore result applied=0x%x", esp_err_to_name(err),
             (int)cause, s_gpio_wake_power_low ? 1 : 0, applied);
    S31SleepCheckpoint(S31SleepPhase::Restored, applied);
    return err;
}

void power_hw_gpio_wake_latch_get(bool* boot_low, bool* power_low) {
    if (boot_low != nullptr) {
        *boot_low = s_gpio_wake_boot_low;
    }
    if (power_low != nullptr) {
        *power_low = s_gpio_wake_power_low;
    }
}

uint32_t power_hw_take_key_wake_latch(void) {
    return s_key_wake_latch.exchange(0);
}
