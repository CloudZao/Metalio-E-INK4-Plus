/**
 * @brief S31 低功耗硬件原子（对齐 light_test 入睡序列）
 */
#include "power_hw.h"
#include "s31_sleep/s31_sleep.h"

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "config.h"
#include "device_state.h"
#include "display.h"
#include "epd_flash_guard.h"
#include "frontlight.h"
#include "metalio_accel.h"
#include "metalio_audio.h"
#include "metalio_keys.h"
#include "metalio_sd.h"
#include "epd_board_metalio_eink4_plus.h"
#include "lv_adapter_display.h"
#include "sd_access.h"
#include "display/font/ttf_convert.h"
#include "standby_screen/standby_screen.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <wifi_station.h>

#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include <hal/usb_serial_jtag_ll.h>
extern "C" bool usb_serial_jtag_is_connected(void);
#endif

#include <atomic>
#include <stdio.h>
#include <esp_rom_sys.h>

#define TAG "PowerHw"

static constexpr uint32_t kPwrOffStack = 16 * 1024; // LVGL 同步绘制栈，与显示任务一致，分配到 PSRAM
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
static constexpr int kUsjReviveDisconnectMs = 150;
static constexpr int kUsjReviveWaitConnectedMs = 800;
#endif

static Nt26Board* CellularBoard() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual ? dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard()) : nullptr;
}

void power_hw_cellular_resume(void) {
    if (auto* cell = CellularBoard()) cell->RecoverAfterPowerOnAsync();
}

static std::atomic<int> s_main_rail_state{-1}; // -1=未知，0=关闭，1=开启；策略与联网任务共享
static bool s_sd_parked = false;
static bool s_gpio_wake_boot_low = false;
static bool s_gpio_wake_power_low = false;

// Keep board-facing entry points stable; SDK details live in s31_sleep.
void power_hw_log_sleep_reset(void) { s31_sleep::CaptureReset(); }
void power_hw_report_sleep_reset(void) { s31_sleep::ReportReset(); }
esp_err_t power_hw_sleep_init(void) { return s31_sleep::Init(); }

static void NotifyNetworkIconChanged(void) {
    Display* disp = Board::GetInstance().GetDisplay();
    if (disp != nullptr) {
        disp->UpdateStatusBar(true);
    }
}

/** 确认共享 INT 线为空闲高 */
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

/** 醒瞬间锁存 P17（须在 restore_keys 前读） */
static bool ReadPowerPinLow(void) {
    return !epd_board_metalio_eink4_plus_ioexp_get_level(EPD_IO_POWER);
}

/** EXT1：GPIO2 低电平唤醒（电源键 P17 → TCA INT） */
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

/**
 * tip 增量阶段。SD/CNNT hold 与 USJ 同域：浅睡可开 kLsSd，但醒后须在 Sd_Init 之后 ReviveUsj。
 */
enum : unsigned {
    kLsSd = 1u << 0,    // HoldPinsLow + Latch + 醒后 Sd_Init
    kLsTps = 1u << 1,   // tps_enter_sleep
    kLsBus = 1u << 2,   // bus_hold_low + latch + 醒后 bus_restore
    kLsRails = 1u << 3, // sleep_cut_rails + 醒后 restore_rails
};
constexpr unsigned kLightSleepHw = kLsSd | kLsTps | kLsBus | kLsRails;

/**
 * @brief SD/CNNT 折腾后恢复副 console（须在 MetalioSd_Init 之后）
 * @note 醒后立刻踢 pad 无效；要等 SDMMC 把 ded_sel 拉回后再软拔插逼主机重枚举
 */
static void ReviveUsjAfterSdRestore(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    usb_serial_jtag_ll_enable_bus_clock(true);
    usb_serial_jtag_ll_phy_enable_pad(false);
    vTaskDelay(pdMS_TO_TICKS(kUsjReviveDisconnectMs));
    usb_serial_jtag_ll_phy_set_defaults();
    usb_serial_jtag_ll_phy_enable_pad(true);

    int waited = 0;
    while (waited < kUsjReviveWaitConnectedMs && !usb_serial_jtag_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    esp_rom_printf("PowerHw: usj revive after SD conn=%d wait=%d\n",
                   usb_serial_jtag_is_connected() ? 1 : 0, waited);
    ESP_LOGI(TAG, "usj revive after SD conn=%d waited=%dms",
             usb_serial_jtag_is_connected() ? 1 : 0, waited);
    fflush(stdout);
#endif
}

esp_err_t power_hw_sd_park(void) {
    sd_access::Transition transition;
    if (!transition) return ESP_ERR_TIMEOUT;
    if (s_sd_parked) return ESP_OK;
    if (!sd_access::IsQuiescing() || !sd_access::IsIdle()) return ESP_ERR_INVALID_STATE;
    if (!MetalioSd_HoldPinsLow()) return ESP_FAIL;
    MetalioSd_LatchForSleep();
    s_sd_parked = true;
    return ESP_OK;
}

esp_err_t power_hw_sd_restore(void) {
    sd_access::Transition transition;
    if (!transition) return ESP_ERR_TIMEOUT;
    if (!s_sd_parked) return ESP_OK;
    const bool mounted = MetalioSd_Init();
    if (mounted) s_sd_parked = false;
    ReviveUsjAfterSdRestore();
    return mounted ? ESP_OK : ESP_FAIL;
}

static void SleepAbort(const char* why, unsigned applied) {
    ESP_LOGW(TAG, "%s (applied=0x%x)", why, applied);
    if ((applied & kLsRails) != 0) {
        epd_board_metalio_eink4_plus_sleep_restore_rails();
        MetalioAudio_OnPowerRestored();
        if (auto* cell = CellularBoard()) cell->OnPowerRestored();
        Frontlight::GetInstance().ForceAllLow();
    }
    if ((applied & kLsBus) != 0) {
        epd_board_metalio_eink4_plus_bus_restore();
    }
    MetalioKeys_ResumeAfterSleep();
    if ((applied & kLsSd) != 0) {
        power_hw_sd_restore();
    }
}

esp_err_t power_hw_pa_set(bool on) {
    MetalioAudio_SetPa(on);
    return ESP_OK;
}

esp_err_t power_hw_bt_power_reset(void) {
    auto* cell = CellularBoard();
    if (cell) cell->PreparePowerOff();
    esp_err_t err;
    {
        // 等当前波形结束，并阻止 P14 断电期间探测/启动 TPS。
        // 4G 清理与恢复可能更新 UI，必须放在此锁之外。
        EpdFlashGuard panel_guard;
        MetalioAudio_SetPa(false);
        MetalioAudio_OnPowerOff();
        err = epd_board_metalio_eink4_plus_bt_power_reset();
        if (err == ESP_OK) {
            // 与开机 P14 恢复路径一致，待电源稳定后才允许下一次刷屏。
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
    if (err == ESP_OK) {
        MetalioAudio_OnPowerRestored();
        if (cell) {
            cell->OnPowerRestored();
            cell->RecoverAfterPowerOnAsync();
        }
    }
    return err;
}

esp_err_t power_hw_main_rail_set(bool on) {
    // S31 无独立 MAIN_PWR；保持策略态幂等
    const int next = on ? 1 : 0;
    if (s_main_rail_state.exchange(next) == next) {
        return ESP_OK;
    }
    if (on) power_hw_cellular_resume();
    ESP_LOGD(TAG, "main_rail_set(%d) policy only; physical P14 follows light sleep", on ? 1 : 0);
    return ESP_OK;
}

bool power_hw_main_rail_is_on(void) {
    return s_main_rail_state.load() != 0;
}

esp_err_t power_hw_wifi_stop(void) {
    if (CellularBoard()) return ESP_OK;
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi STA pause skipped (not started)");
        return ESP_OK;
    }
    WifiStation::GetInstance().PauseForLp();
    ESP_LOGI(TAG, "WiFi STA paused for LP");
    NotifyNetworkIconChanged();
    return ESP_OK;
}

esp_err_t power_hw_wifi_start(void) {
    if (CellularBoard()) { power_hw_cellular_resume(); return ESP_OK; }
    auto& sta = WifiStation::GetInstance();
    if (!sta.IsUserEnabled()) return ESP_OK;
    wifi_mode_t ap_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&ap_mode) == ESP_OK && (ap_mode & WIFI_MODE_AP) != 0 &&
        (ap_mode & WIFI_MODE_STA) == 0) {
        ESP_LOGI(TAG, "WiFi STA start skipped (softAP)");
        return ESP_OK;
    }
    if (sta.IsLpPaused()) {
        esp_err_t err = sta.ResumeFromLp();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi STA resumed from LP");
            NotifyNetworkIconChanged();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi STA resume failed (%s), full Start", esp_err_to_name(err));
        sta.Start();
        ESP_LOGI(TAG, "WiFi STA started");
        NotifyNetworkIconChanged();
        return ESP_OK;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already started");
        return ESP_OK;
    }
    sta.Start();
    ESP_LOGI(TAG, "WiFi STA started");
    NotifyNetworkIconChanged();
    return ESP_OK;
}

static StaticSemaphore_t s_cpu_freq_mutex_storage;
static SemaphoreHandle_t s_cpu_freq_mutex = xSemaphoreCreateMutexStatic(&s_cpu_freq_mutex_storage);
static unsigned s_cpu_freq_holds = 0;
static int s_cpu_freq_requested = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

static esp_err_t ApplyCpuFreq(int mhz) {
#if CONFIG_PM_ENABLE
    if (mhz < 10) {
        mhz = 10;
    }
    esp_pm_config_t cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    ESP_LOGI(TAG, "cpu %dMHz (no auto_ls): %s", mhz, esp_err_to_name(err));
    return err;
#else
    (void)mhz;
    ESP_LOGW(TAG, "cpu freq skipped (CONFIG_PM_ENABLE off)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t power_hw_cpu_freq_set(int mhz) {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    const esp_err_t err = s_cpu_freq_holds ? ESP_OK : ApplyCpuFreq(mhz);
    if (err == ESP_OK) s_cpu_freq_requested = mhz;
    xSemaphoreGive(s_cpu_freq_mutex);
    return err;
}

PowerHwCpuFreqHold::PowerHwCpuFreqHold() {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    if (s_cpu_freq_holds == 0) {
        #if CONFIG_PM_ENABLE
        ESP_ERROR_CHECK(ApplyCpuFreq(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
        #endif
    }
    ++s_cpu_freq_holds;
    xSemaphoreGive(s_cpu_freq_mutex);
}

PowerHwCpuFreqHold::~PowerHwCpuFreqHold() {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    if (--s_cpu_freq_holds == 0) {
        ApplyCpuFreq(s_cpu_freq_requested);
    }
    xSemaphoreGive(s_cpu_freq_mutex);
}

static void PwrOffTask(void* /*arg*/) {
    ttf_convert::Cancel();
    while (ttf_convert::Busy()) vTaskDelay(pdMS_TO_TICKS(20));
    power_hw_pa_set(false);
    if (power_hw_sd_restore() != ESP_OK) {
        ESP_LOGW(TAG, "power-off: SD restore failed, using default wallpaper");
    }
    if (auto* disp = LVAdapterDisplay::Instance()) {
        while (!disp->ShowPoweredOffScreen()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    Frontlight::GetInstance().ForceAllLow();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "power-off: PWR_KEY pulse train");
    for (;;) {
        epd_board_metalio_eink4_plus_pwr_key_pulse_train();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void power_hw_begin_power_off(void) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    ESP_LOGW(TAG, "begin power off");
    if (xTaskCreatePinnedToCoreWithCaps(PwrOffTask, "pwr_off", kPwrOffStack, nullptr,
                                      tskIDLE_PRIORITY + 5, nullptr, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "pwr_off task create failed");
        started = false;
    }
}

/**
 * tip 全量浅睡；SD/CNNT hold 保留。醒后：restore → Sd_Init →（若做过 SD）ReviveUsj。
 */
esp_err_t power_hw_light_sleep_once(uint64_t max_sleep_us) {
    // No timeout may force power removal with live files or boot hardware work.
    if (!sd_access::IsQuiescing() || !sd_access::IsIdle() ||
        Application::GetInstance().IsBootWorkActive() || StandbyScreen::HasWakeRequest()) {
        return ESP_ERR_INVALID_STATE;
    }
    sd_access::Transition transition;
    if (!transition) return ESP_ERR_TIMEOUT;
    unsigned applied = 0;
    s31_sleep::BeginCycle();
    s31_sleep::Checkpoint(s31_sleep::Phase::Admission, applied); // admission passed
    ESP_LOGI(TAG, "light sleep once max_us=%llu hw=0x%x (sd=%d tps=%d bus=%d rails=%d)",
             (unsigned long long)max_sleep_us, kLightSleepHw,
             (kLightSleepHw & kLsSd) ? 1 : 0, (kLightSleepHw & kLsTps) ? 1 : 0,
             (kLightSleepHw & kLsBus) ? 1 : 0, (kLightSleepHw & kLsRails) ? 1 : 0);

    if ((kLightSleepHw & kLsSd) != 0) {
        if (power_hw_sd_park() != ESP_OK) return ESP_ERR_INVALID_STATE;
        applied |= kLsSd;
    }
    if ((kLightSleepHw & kLsTps) != 0) {
        epd_board_metalio_eink4_plus_tps_enter_sleep();
        applied |= kLsTps;
    }
    if ((kLightSleepHw & kLsBus) != 0) {
        epd_board_metalio_eink4_plus_bus_hold_low();
        applied |= kLsBus;
    }

    MetalioAccel_ReleaseIntLine();
    s31_sleep::Checkpoint(s31_sleep::Phase::HardwareParked, applied); // SD/TPS/bus parked
    MetalioKeys_MaskForSleep();
    if (MetalioKeys_HasPowerClick() || StandbyScreen::HasWakeRequest()) {
        SleepAbort("abort light sleep: pending POWER exit", applied);
        return ESP_ERR_INVALID_STATE;
    }

    if (!WaitIntLineHigh()) {
        SleepAbort("abort light sleep: GPIO2 low", applied);
        return ESP_ERR_INVALID_STATE;
    }
    if (!epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin()) {
        SleepAbort("abort light sleep: P17 input fail", applied);
        return ESP_ERR_INVALID_STATE;
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (!EnableGpio2Wake()) {
        SleepAbort("abort light sleep: wake cfg fail", applied);
        return ESP_ERR_INVALID_STATE;
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
        s31_sleep::Checkpoint(s31_sleep::Phase::WakeArmed, applied); // wake sources armed; about to cut rails
        Frontlight::GetInstance().ForceAllLow();
        if (auto* cell = CellularBoard()) cell->PreparePowerOff();
        MetalioAudio_OnPowerOff();
        epd_board_metalio_eink4_plus_sleep_cut_rails();
        applied |= kLsRails;
    }
    if ((kLightSleepHw & kLsBus) != 0) {
        epd_board_metalio_eink4_plus_bus_latch_for_sleep();
    }

    fflush(stdout);
    s31_sleep::Checkpoint(s31_sleep::Phase::BeforeSleep, applied); // last checkpoint before the CPU sleeps
    esp_err_t err = s31_sleep::LightSleep(applied);
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // 醒瞬间锁存 P17；此时 USJ 可能仍哑，用 ROM 打一行便于对照
    s_gpio_wake_boot_low = false;
    s_gpio_wake_power_low = ReadPowerPinLow();
    esp_rom_printf("PowerHw: light_sleep wake err=%s cause=%d power_low=%d\n",
                   esp_err_to_name(err), (int)cause, s_gpio_wake_power_low ? 1 : 0);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    if (max_sleep_us > 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    MetalioKeys_ResumeAfterSleep();
    s31_sleep::Checkpoint(s31_sleep::Phase::KeysRestored, applied); // key/IO-expander access returned
    if ((applied & kLsRails) != 0) {
        epd_board_metalio_eink4_plus_sleep_restore_rails();
        MetalioAudio_OnPowerRestored();
        if (auto* cell = CellularBoard()) cell->OnPowerRestored();
        // P14 回来时 LEDC 可能仍有旧占空比 → 背光闪一下；待机/关机路径保持灭
        Frontlight::GetInstance().ForceAllLow();
    }
    if ((applied & kLsSd) != 0 && cause != ESP_SLEEP_WAKEUP_TIMER) {
        s31_sleep::Checkpoint(s31_sleep::Phase::BeforeSdRestore, applied); // rails restored; about to restore SD/USB console
        power_hw_sd_restore();
    }
    if ((applied & kLsBus) != 0) {
        s31_sleep::Checkpoint(s31_sleep::Phase::BeforeBusRestore, applied); // about to reinitialize display bus
        epd_board_metalio_eink4_plus_bus_restore();
    }
    ESP_LOGI(TAG, "light_sleep wake err=%s cause=%d power_low=%d restore done applied=0x%x",
             esp_err_to_name(err), (int)cause, s_gpio_wake_power_low ? 1 : 0, applied);
    s31_sleep::Checkpoint(s31_sleep::Phase::Restored, applied); // hardware restore complete
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
