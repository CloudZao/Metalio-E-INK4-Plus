#include "plus_hal.h"
#include "asset_contract.h"
#include "app_assets.h"
#include "mono_frame.h"
#include "power_key.h"
#include "wake_hold.h"
#include "ai_key.h"
#include "volume_key.h"
#include "touch_edges.h"
#include "config.h"
#include "metalio_epd.h"
#include "metalio_touch.h"
#include "metalio_i2c1.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"
#include "cx25601n.h"
#include "bq27220_gauge.h"
#include "power_i2c.h"
// The inherited C driver's header does not provide its own C++ linkage guard.
extern "C" {
#include "pca9555.h"
}

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/rtc_io.h>
#include <driver/sdmmc_host.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_private/esp_cache_private.h>
#include <esp_psram.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>
#include <soc/soc_caps.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

extern const uint8_t app_lock_start[] asm("_binary_app_lockscreen_bin_start");
extern const uint8_t app_lock_end[] asm("_binary_app_lockscreen_bin_end");
extern const uint8_t app_deep_start[] asm("_binary_app_deep_sleep_bin_start");
extern const uint8_t app_deep_end[] asm("_binary_app_deep_sleep_bin_end");

namespace {
using namespace plus_hal_detail;
constexpr char kTag[] = "PlusHal";
constexpr uint32_t kPulseMs = 30;
constexpr uint32_t kFrontlightHz = 20000;
// Official Plus UI's minimum nonzero level: 1023 * 1 * 20 / 10000 = 2.
// Cool only avoids doubling the lowest configured luminous output.
constexpr uint32_t kFrontlightDimDuty = 2;
#ifndef PLUS_APP_WAKE_MAGIC
#error PLUS_APP_WAKE_MAGIC must be generated for this app identity
#endif
constexpr uint32_t kDeepSleepMagic = PLUS_APP_WAKE_MAGIC;
RTC_DATA_ATTR uint32_t s_deep_sleep_magic = 0;
RTC_DATA_ATTR uint8_t s_sleep_tca_addr = 0x20;

portMUX_TYPE s_activity_mux = portMUX_INITIALIZER_UNLOCKED;
uint64_t s_activity_us = 0;
void RecordActivity(uint64_t now) {
    portENTER_CRITICAL(&s_activity_mux);
    if (now > s_activity_us) s_activity_us = now;
    portEXIT_CRITICAL(&s_activity_mux);
}
std::atomic<bool> s_ready{false}, s_busy{false}, s_healthy{false};
std::atomic<bool> s_app_healthy{true};
std::atomic<bool> s_pause_requested{false}, s_powered_down{false};
std::atomic<bool> s_standby{false}, s_power_off_event{false};
std::atomic<bool> s_control_busy{false};
std::atomic<uint32_t> s_control_requested{0}, s_control_handled{0};
std::atomic<plus_hal::LockscreenStyle> s_lockscreen_style{plus_hal::LockscreenStyle::Application};
std::atomic<uint32_t> s_standby_revision{0}, s_power_epoch{0};
std::atomic<int> s_volume_delta{0};
std::atomic<uint32_t> s_volume_epoch{0}, s_volume_event_epoch{0};
std::atomic<uint32_t> s_displayed_revision{0};
std::atomic<TaskHandle_t> s_flash_owner{nullptr};
bool s_init_attempted = false, s_panel_initialized = false;
std::string s_panel_init_error;
bool s_flash_i2c_held = false;  // Only the task owning both gates may access.
SemaphoreHandle_t s_frame_gate = nullptr, s_panel_gate = nullptr;
SemaphoreHandle_t s_aux_gate = nullptr, s_init_done = nullptr;
TaskHandle_t s_display_task = nullptr, s_motor_task = nullptr, s_power_task = nullptr;
bool s_frontlight_ready = false;
// Stacks/TCBs remain in internal RAM when a caller disables Flash cache.
DRAM_ATTR StaticTask_t s_display_tcb, s_motor_tcb, s_power_tcb;
DRAM_ATTR StackType_t s_display_stack[8192 / sizeof(StackType_t)];
DRAM_ATTR StackType_t s_motor_stack[3072 / sizeof(StackType_t)];
DRAM_ATTR StackType_t s_power_stack[4096 / sizeof(StackType_t)];
uint8_t* s_pending = nullptr;
int s_pending_width = 0, s_pending_height = 0;
uint32_t s_pending_revision = 0;
bool s_pending_full = false;
bool s_pending_is_gray = false;
plus_hal::DisplayMode s_pending_mode = plus_hal::DisplayMode::Auto;

sdmmc_card_t* s_card = nullptr;
void* s_sd_dma = nullptr;
std::atomic<bool> s_tf_mounted{false};

TouchEdges<64> s_touch_queue;
portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_pointer_pressed = false, s_pointer_seen = false;
bool s_replay_press = false, s_replay_release = false;
int s_pointer_x = 0, s_pointer_y = 0;
std::atomic<int> s_input_consumer{0};  // 1=edge FIFO, 2=pointer snapshots.
QueueHandle_t s_motor_queue = nullptr;
std::atomic<uint32_t> s_motor_epoch{1};
std::atomic<uint32_t> s_motor_requested{0}, s_motor_queued{0};
std::atomic<uint32_t> s_motor_started{0}, s_motor_finished{0}, s_motor_dropped{0};
std::atomic<uint32_t> s_motor_max_delay_us{0};
std::atomic<uint32_t> s_touch_samples_during_refresh{0};

bool Fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    ESP_LOGE(kTag, "%s", reason);
    return false;
}

TickType_t RemainingTicks(int64_t deadline) {
    const int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) return 0;
    return std::max<TickType_t>(1, pdMS_TO_TICKS(uint32_t((remaining + 999) / 1000)));
}

bool TakeUntil(SemaphoreHandle_t gate, int64_t deadline) {
    return gate && xSemaphoreTake(gate, RemainingTicks(deadline)) == pdTRUE;
}

void ClearPointerLocked() {
    s_pointer_pressed = false;
    s_pointer_seen = false;
    s_replay_press = false;
    s_replay_release = false;
}

// s_standby is only the requested target. Until the worker has physically
// restored the app image, the public/input state must remain locked too.
bool ScreenInputBlocked() {
    return s_standby.load(std::memory_order_acquire) ||
           s_control_busy.load(std::memory_order_acquire) ||
           s_control_requested.load(std::memory_order_acquire) !=
           s_control_handled.load(std::memory_order_acquire);
}

bool MotorBlocked() {
    return !s_ready.load() || s_pause_requested.load() || s_powered_down.load() ||
           ScreenInputBlocked();
}

void QueueMotorRequest(const MotorRequest& request) {
    ++s_motor_requested;
    if (!s_motor_queue || !MotorRequestLive(request, s_motor_epoch.load(), MotorBlocked()) ||
        xQueueSend(s_motor_queue, &request, 0) != pdTRUE) {
        ++s_motor_dropped;
        return;
    }
    ++s_motor_queued;
}

// Flash and power boundaries invalidate queued feedback. Ordinary navigation
// only fences touch coordinates and does not cancel its just-received pulse.
void CancelMotorRequests() {
    s_motor_epoch.fetch_add(1, std::memory_order_acq_rel);
    MotorRequest request{};
    while (s_motor_queue && xQueueReceive(s_motor_queue, &request, 0) == pdTRUE)
        ++s_motor_dropped;
}

// Called by the verified FT6336U feed task after successful I2C sampling.
// Cover keys (raw Y around 1600) must never become a clamped screen press.
void TouchSample(uint8_t count, int raw_x, int raw_y, int lx, int ly) {
    const uint32_t motor_epoch = s_motor_epoch.load(std::memory_order_acquire);
    const int64_t sampled_us = esp_timer_get_time();
    if (s_busy.load(std::memory_order_relaxed)) ++s_touch_samples_during_refresh;
    bool feedback = false;
    const bool confirmed_release = !count && metalio_touch_sample_confirms_release();
    portENTER_CRITICAL(&s_touch_mux);
    const bool blocked = !metalio_touch_sample_is_current() ||
                         !s_ready.load() || s_pause_requested.load() ||
                         s_powered_down.load() || ScreenInputBlocked();
    const bool in_panel = raw_x >= 0 && raw_y >= 0 && raw_x < kWidth && raw_y < kWidth;
    const auto sample = s_touch_queue.Sample(count != 0, in_panel,
                                           std::clamp(lx, 0, plus_hal::kLogicalWidth - 1),
                                           std::clamp(ly, 0, plus_hal::kLogicalHeight - 1),
                                           confirmed_release, blocked, sampled_us);
    if (blocked || s_touch_queue.WaitingRelease()) {
        ClearPointerLocked();
    } else if (!count) {
        if (s_pointer_pressed && !s_pointer_seen) {
            s_replay_press = true;
            s_replay_release = true;
        }
        s_pointer_pressed = false;
        s_pointer_seen = false;
    } else if (in_panel) {
            s_pointer_x = std::clamp(lx, 0, plus_hal::kLogicalWidth - 1);
            s_pointer_y = std::clamp(ly, 0, plus_hal::kLogicalHeight - 1);
            if (sample.new_down) {
                s_pointer_seen = false;
                s_replay_press = false;
                s_replay_release = false;
            }
            s_pointer_pressed = true;
    }
    feedback = sample.new_down;
    portEXIT_CRITICAL(&s_touch_mux);
    // A legal new DOWN gets one feedback request even if the business FIFO is
    // full. Feedback means physical reception, not successful business action.
    // Touch overflow and motor-queue rejection are independently diagnostic.
    if (feedback) { RecordActivity(uint64_t(sampled_us)); QueueMotorRequest({motor_epoch, sampled_us}); }
}

bool SelectInputConsumer(int wanted) {
    int expected = 0;
    s_input_consumer.compare_exchange_strong(expected, wanted);
    return s_input_consumer.load() == wanted;
}

bool SdBufferAligned(int slot, const void* buffer, size_t bytes) {
    return esp_ptr_internal(buffer) && sdmmc_host_check_buffer_alignment(slot, buffer, bytes);
}

void ReleaseSdHolds() {
    const gpio_num_t pins[] = {SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN,
                              SDMMC_D1_PIN, SDMMC_D2_PIN, SDMMC_D3_PIN};
    for (auto pin : pins) gpio_hold_dis(pin);
}

void MountTf() {
    ReleaseSdHolds();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    size_t alignment = 0;
    if (esp_cache_get_alignment(caps, &alignment) != ESP_OK) {
        ESP_LOGW(kTag, "TF DMA alignment unavailable");
        return;
    }
    alignment = std::max<size_t>(alignment, 4);
    // Retain the accepted S31 4096-byte internal bounce-buffer contract.
    s_sd_dma = heap_caps_aligned_alloc(alignment, 4096, caps);
    if (!s_sd_dma) {
        ESP_LOGW(kTag, "optional TF DMA buffer allocation failed");
        return;
    }
    host.check_buffer_alignment = SdBufferAligned;
    host.dma_aligned_buffer = s_sd_dma;
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SDMMC_CLK_PIN; slot.cmd = SDMMC_CMD_PIN;
    slot.d0 = SDMMC_D0_PIN; slot.d1 = SDMMC_D1_PIN;
    slot.d2 = SDMMC_D2_PIN; slot.d3 = SDMMC_D3_PIN;
    slot.width = 4; slot.flags = 0;
    esp_vfs_fat_sdmmc_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 12;
    config.allocation_unit_size = 16 * 1024;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &s_card);
    s_tf_mounted.store(err == ESP_OK);
    if (err != ESP_OK) {
        s_card = nullptr;
        heap_caps_free(s_sd_dma); s_sd_dma = nullptr;
        ESP_LOGW(kTag, "optional TF mount: %s (never format)", esp_err_to_name(err));
    } else ESP_LOGI(kTag, "TF ready: /sdcard 4-bit, 4096-byte internal DMA bounce");
}

void InitCharging() {
    power_i2c_t bus{};
    bus.port = I2C_NUM_1;
    bus.lock = metalio_i2c1_lock;
    bus.unlock = metalio_i2c1_unlock;
    bus.device = metalio_i2c1_device_locked;
    Bq27220Gauge::GetInstance().Begin(&bus);
    if (power_i2c_probe(&bus, CX25601N_I2C_ADDR, 100) != ESP_OK) {
        ESP_LOGW(kTag, "CX25601N unavailable; continuing without charger I2C");
        return;
    }
    esp_err_t err = cx25601n_init(&bus);
    if (err == ESP_OK) err = cx25601n_set_ichg_ma(1000);
    if (err == ESP_OK) err = cx25601n_enable_charge(true);
    if (err != ESP_OK) ESP_LOGW(kTag, "charger setup: %s", esp_err_to_name(err));
    // Battery reads are demand driven and throttled; no extra UI/task imported.
}

void HoldFrontlightDark() {
    // Set the pending GPIO latch before releasing a retained hold. No PWM or
    // display setup is allowed in the early wake gate.
    for (const auto pin : {FL_COOL_PIN, FL_WARM_PIN}) {
        gpio_set_level(pin, 0);
        gpio_hold_dis(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        gpio_hold_en(pin);
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

bool ReadSleepKey(bool* down) {
    if (!down || !metalio_i2c1_lock(100)) return false;
    uint8_t port0 = 0xFF, port1 = 0xFF;
    // Read BOTH ports: an unread change in either keeps open-drain INT low.
    const esp_err_t first = metalio_i2c1_read_reg_stop_locked(
        I2C_NUM_1, s_sleep_tca_addr, 0, &port0, 1, 100);
    const esp_err_t second = metalio_i2c1_read_reg_stop_locked(
        I2C_NUM_1, s_sleep_tca_addr, 1, &port1, 1, 100);
    metalio_i2c1_unlock();
    if (first != ESP_OK || second != ESP_OK) return false;
    *down = !(port1 & (1u << (EPD_IO_POWER & 7)));
    return true;
}

[[noreturn]] void EnterHardwareDeepSleep() {
    // The TCA power-key input, not the touch IRQ, is connected to RTC GPIO2.
    // When a shared source remains low, do NOT arm level wake into a reboot
    // storm: sleep with a timer and retry the dark early gate instead.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    rtc_gpio_init(IO_EXPANDER_INT_GPIO);
    rtc_gpio_set_direction(IO_EXPANDER_INT_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(IO_EXPANDER_INT_GPIO);
    rtc_gpio_pulldown_dis(IO_EXPANDER_INT_GPIO);
    const bool irq_idle = rtc_gpio_get_level(IO_EXPANDER_INT_GPIO) != 0;
    esp_err_t wake = ESP_FAIL;
    if (irq_idle) wake = esp_sleep_enable_ext1_wakeup_io(
        1ULL << IO_EXPANDER_INT_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    if (!irq_idle || wake != ESP_OK) {
        ESP_LOGW(kTag, "DEEP_WAKE timer fallback irq_idle=%d ext1=%s",
                 int(irq_idle), esp_err_to_name(wake));
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(500000));
    }
    metalio_i2c1_deinit();
    HoldFrontlightDark();
    ESP_LOGI(kTag, "DEEP_SLEEP enter wake=%s hold=5000ms", irq_idle && wake == ESP_OK ? "GPIO2" : "timer");
    esp_deep_sleep_start();
    __builtin_unreachable();
}

bool SetFrontlight(bool visible) {
    if (!s_frontlight_ready) return false;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                 visible ? kFrontlightDimDuty : 0);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err == ESP_OK) err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    if (err != ESP_OK) {
        // Fail dark. Keep the LEDC matrix attached so a later wake can retry.
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ESP_LOGE(kTag, "frontlight PWM: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool InitFrontlight(std::string* error) {
    // Pin latches are zero before PWM routing, so setup cannot emit full light.
    gpio_set_direction(FL_COOL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(FL_WARM_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(FL_COOL_PIN, 0);
    gpio_set_level(FL_WARM_PIN, 0);
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = kFrontlightHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return Fail(error, "frontlight PWM timer initialization failed");
    ledc_channel_config_t channel{};
    channel.gpio_num = FL_COOL_PIN;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) return Fail(error, "cool frontlight PWM initialization failed");
    channel.gpio_num = FL_WARM_PIN;
    channel.channel = LEDC_CHANNEL_1;
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) return Fail(error, "warm frontlight PWM initialization failed");
    s_frontlight_ready = true;
    if (!SetFrontlight(true)) return Fail(error, "minimum frontlight PWM failed");
    ESP_LOGI(kTag, "frontlight PWM=%luHz/10bit cool=%lu/1023 warm=0",
             (unsigned long)kFrontlightHz, (unsigned long)kFrontlightDimDuty);
    return true;
}

// PowerTask owns s_aux_gate here; Flash must drain it before taking I2C.
bool SetStandbyLocked(bool standby) {
    xSemaphoreTake(s_frame_gate, portMAX_DELAY);
    if (s_pause_requested.load() || s_powered_down.load()) {
        xSemaphoreGive(s_frame_gate);
        return false;
    }
    // Only publish desired state here. The display worker drains any accepted
    // app frame before its internal overlay/restoration; never take panel while
    // holding aux (Flash owns panel before aux).
    s_volume_epoch.fetch_add(1, std::memory_order_acq_rel);
    s_volume_delta.store(0, std::memory_order_release);
    CancelMotorRequests();
    epd_board_metalio_eink4_plus_motor_set(false);
    if (standby) s_standby.store(true, std::memory_order_release);
    const bool light_ok = SetFrontlight(!standby);
    plus_hal::InvalidateTouches();
    if (!standby) s_standby.store(false, std::memory_order_release);
    s_standby_revision.fetch_add(1, std::memory_order_release);
    s_control_requested.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(s_frame_gate);
    if (s_display_task) xTaskNotifyGive(s_display_task);
    ESP_LOGI(kTag, "STANDBY %s light=%s RAM=retained accepted_frame=%lu",
             standby ? "ENTER" : "WAKE", light_ok ? "ok" : "failed-dark",
             (unsigned long)s_displayed_revision.load());
    return true;
}

void PowerTask(void*) {
    PowerKeyState key;
    VolumeKeyState volume;
    int64_t last_volume_sample = -1;
    uint32_t epoch = s_power_epoch.load();
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // Power-key sampling must not be starved by long/continuous refreshes.
        // Its short synchronous TCA read shares I2C ownership with TPS/motor;
        // no LCD frame/PMIC timing or waveform is changed.
        if (!s_ready.load() || s_pause_requested.load() || s_powered_down.load()) {
            key.ResetUnknown();
            volume.ResetUnknown(); s_volume_delta.store(0, std::memory_order_release);
        } else if (xSemaphoreTake(s_aux_gate, pdMS_TO_TICKS(10)) == pdTRUE) {
            const uint32_t current_epoch = s_power_epoch.load(std::memory_order_acquire);
            if (current_epoch != epoch) { key.ResetUnknown(); epoch = current_epoch; }
            if (s_pause_requested.load() || s_powered_down.load()) {
                key.ResetUnknown();
                volume.ResetUnknown(); s_volume_delta.store(0, std::memory_order_release);
            } else {
                uint8_t port1 = 0xFF;
                const bool ok = epd_board_metalio_eink4_plus_ioexp_read_port(1, &port1);
                if (!ok || s_pause_requested.load()) {
                    key.ResetUnknown(); volume.ResetUnknown(); s_volume_delta.store(0, std::memory_order_release);
                }
                else {
                    const int64_t sampled_at = esp_timer_get_time();
                    if (last_volume_sample >= 0 && sampled_at - last_volume_sample > 120000)
                        s_volume_delta.store(0, std::memory_order_release);
                    last_volume_sample = sampled_at;
                    const bool down = !(port1 & (1u << (EPD_IO_POWER & 7)));
                    if (down) RecordActivity(uint64_t(sampled_at));
                    const PowerKeyEvent event = key.Sample(down, sampled_at);
                    if (event == PowerKeyEvent::ShortPress) {
                        SetStandbyLocked(!s_standby.load());
                    } else if (event == PowerKeyEvent::LongPress) {
                        // Save in the app, then call PowerOff. A long hold in
                        // lock mode must never relight or restore the app first.
                        s_power_off_event.store(true, std::memory_order_release);
                        ESP_LOGI(kTag, "POWER_LONG_PRESS hold=5000ms");
                    }
                    const auto volume_epoch = s_volume_epoch.load(std::memory_order_acquire);
                    const bool volume_blocked = s_pause_requested.load() || s_powered_down.load() ||
                        s_power_off_event.load() || ScreenInputBlocked();
                    const bool up_down = !(port1 & (1u << (EPD_IO_VOL_UP & 7)));
                    const bool down_down = !(port1 & (1u << (EPD_IO_VOL_DOWN & 7)));
                    if (!volume_blocked && (up_down || down_down)) RecordActivity(uint64_t(sampled_at));
                    const int step = volume.Sample(up_down, down_down, volume_blocked, volume_epoch, sampled_at);
                    if (volume_blocked || (up_down && down_down) ||
                        s_volume_event_epoch.load(std::memory_order_acquire) != volume_epoch) {
                        s_volume_delta.store(0, std::memory_order_release);
                        s_volume_event_epoch.store(volume_epoch, std::memory_order_release);
                    }
                    if (step && !volume_blocked && s_volume_epoch.load(std::memory_order_acquire) == volume_epoch) {
                        int previous = s_volume_delta.load(std::memory_order_relaxed);
                        while (!s_volume_delta.compare_exchange_weak(previous, std::clamp(previous + step, -12, 12),
                                                                    std::memory_order_release, std::memory_order_relaxed)) {}
                    }
                }
            }
            xSemaphoreGive(s_aux_gate);
        } // A short motor lease can skip one sample; Sample disarms on >120ms gap.
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));
    }
}

void MotorTask(void*) {
    for (;;) {
        MotorRequest request{};
        if (xQueueReceive(s_motor_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_aux_gate, portMAX_DELAY);
        if (MotorRequestLive(request, s_motor_epoch.load(), MotorBlocked())) {
            epd_board_metalio_eink4_plus_motor_set(true);
            ++s_motor_started;
            const int64_t delay = esp_timer_get_time() - request.requested_us;
            const uint32_t value = uint32_t(std::min<int64_t>(std::max<int64_t>(0, delay), UINT32_MAX));
            uint32_t previous = s_motor_max_delay_us.load();
            while (value > previous && !s_motor_max_delay_us.compare_exchange_weak(previous, value)) {}
            vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(kPulseMs)));
            epd_board_metalio_eink4_plus_motor_set(false);
            ++s_motor_finished;
        } else ++s_motor_dropped;
        xSemaphoreGive(s_aux_gate);
        // Do not merge pending requests. Give the power task its aux lease and
        // hold a real off interval so quick pulses do not become one long buzz.
        const int64_t off_until = esp_timer_get_time() + 10000;
        while (esp_timer_get_time() < off_until) vTaskDelay(1);
    }
}

EpdDrawError PresentNative(EpdiyHighlevelState* hl, bool requested_full,
                          bool& state_known, unsigned& partials,
                          const char* kind, uint32_t revision,
                          plus_hal::DisplayMode mode=plus_hal::DisplayMode::Auto) {
    const int64_t started = esp_timer_get_time();
    const bool changed = std::memcmp(hl->front_fb, hl->back_fb, kGrayBytes) != 0;
    bool gray_transition=false;
    if(changed) for(size_t i=0;i<kGrayBytes && !gray_transition;++i) {
        const uint8_t a=hl->front_fb[i], b=hl->back_fb[i];
        for(unsigned n: {0u,4u}) { const auto v=(a>>n)&15, old=(b>>n)&15;
            if(v!=old && ((v!=0 && v!=15)||(old!=0 && old!=15))) gray_transition=true; }
    }
    // Browser reading policy: clean once on startup, then DU until the user
    // explicitly requests Ink. Unknown hardware state still needs recovery.
    bool full = requested_full || !state_known || mode==plus_hal::DisplayMode::GrayQuality;
    const bool use_gray=gray_transition || mode==plus_hal::DisplayMode::GrayFast;
    EpdDrawError err = EPD_DRAW_SUCCESS;
    if (changed || full) {
        // Preserve verified PMIC timing, first-clear/GC16, normal DU, and one
        // bounded clear/GC16 retry. Only the display worker touches native FBs.
        if (!metalio_i2c1_lock(1000)) err = EPD_DRAW_EMPTY_LINE_QUEUE;
        else {
            metalio_i2c1_unlock();
            epd_poweron();
            if (!epd_board_metalio_eink4_plus_tps_ok()) err = EPD_DRAW_EMPTY_LINE_QUEUE;
            else {
                if (!state_known || requested_full) {
                    err = epd_clear_checked();
                    if (err == EPD_DRAW_SUCCESS)
                        err = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
                } else if(mode==plus_hal::DisplayMode::GrayQuality)
                    err=epd_hl_update_screen_full(hl,MODE_GC16,25);
                else err = epd_hl_update_screen(hl, use_gray ? MODE_GL16 : MODE_DU, 25);
                if (err != EPD_DRAW_SUCCESS) {
                    ESP_LOGW(kTag, "%s=%lu result=0x%x; one clear/GC16 retry",
                             kind, (unsigned long)revision, unsigned(err));
                    full = true;
                    err = epd_clear_checked();
                    if (err == EPD_DRAW_SUCCESS)
                        err = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
                }
            }
            epd_poweroff();
        }
    }
    state_known = err == EPD_DRAW_SUCCESS;
    if (state_known && (changed || full)) {
        if (full) partials = 0;
        else ++partials;
    }
    s_healthy.store(state_known && !s_powered_down.load(), std::memory_order_release);
    ESP_LOGI(kTag, "%s=%lu mode=%s changed=%d result=0x%x partials=%u elapsed=%lldms",
             kind, (unsigned long)revision, full ? "GC16" : use_gray ? "GL16" : "DU", int(changed),
             unsigned(err), partials, (long long)((esp_timer_get_time() - started) / 1000));
    return err;
}

void DisplayTask(void*) {
    s_panel_initialized = MetalioEpd_Init();
    if (!s_panel_initialized) s_panel_init_error = "EPD/TPS65185 initialization failed";
    s_healthy.store(s_panel_initialized);
    xSemaphoreGive(s_init_done);
    if (!s_panel_initialized) for (;;) vTaskSuspend(nullptr);
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    bool state_known = false;
    unsigned partials = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            // Flash takes panel before aux; this worker NEVER takes aux. The
            // power task only publishes desired state and never waits on panel.
            xSemaphoreTake(s_panel_gate, portMAX_DELAY);
            xSemaphoreTake(s_frame_gate, portMAX_DELAY);
            const bool app_frame = s_busy.load(std::memory_order_acquire);
            const uint32_t control = s_control_requested.load(std::memory_order_acquire);
            if (s_powered_down.load() || (!app_frame &&
                (s_pause_requested.load() || control == s_control_handled.load()))) {
                xSemaphoreGive(s_frame_gate);
                xSemaphoreGive(s_panel_gate);
                break;
            }
            // An accepted business frame always finishes before a later lock
            // overlay. Control epochs coalesce to the latest desired state.
            const uint32_t revision = app_frame ? s_pending_revision : control;
            const bool locked = s_standby.load(std::memory_order_acquire);
            // Lock/wake are ordinary static changes under the same DU policy.
            const bool full = app_frame ? s_pending_full : false;
            const auto mode=app_frame?s_pending_mode:plus_hal::DisplayMode::Auto;
            if (!app_frame) s_control_busy.store(true, std::memory_order_release);
            xSemaphoreGive(s_frame_gate);

            if (!app_frame && locked) {
                plus_assets::Decode(app_assets::Lock, app_lock_start, size_t(app_lock_end - app_lock_start), hl->front_fb, kGrayBytes);
            } else if (SupportedSize(s_pending_width, s_pending_height)) {
                // The single owned mono slot remains a complete app snapshot
                // after completion; it is never overwritten by lock graphics.
                if (s_pending_is_gray) std::memcpy(hl->front_fb, s_pending, kGrayBytes);
                else ExpandMono(s_pending, s_pending_width, s_pending_height, hl->front_fb);
            } else std::memset(hl->front_fb, 0xFF, kGrayBytes);
            const char* kind = app_frame ? "frame" : locked ? "LOCKSCREEN" : "RESTORE_APP";
            const auto err = PresentNative(hl, full, state_known, partials, kind, revision,mode);
            if (app_frame) {
                s_app_healthy.store(err == EPD_DRAW_SUCCESS, std::memory_order_release);
                if (err == EPD_DRAW_SUCCESS)
                    s_displayed_revision.store(revision, std::memory_order_release);
                s_busy.store(false, std::memory_order_release);
            } else {
                // This is a control attempt receipt, not an app presentation
                // receipt. Failed control refresh must not permanently wedge
                // admission; its error stays visible through DisplayHealthy.
                if (!locked && err == EPD_DRAW_SUCCESS)
                    plus_hal::InvalidateTouches();
                s_control_handled.store(control, std::memory_order_release);
                s_control_busy.store(false, std::memory_order_release);
            }
            xSemaphoreGive(s_panel_gate);
            // A newer accepted app/control request may have arrived while ink
            // was running. Reconcile it even if notifications were coalesced.
        }
    }
}
} // namespace

extern "C" void MetalioSd_ReleaseHold(void) { ReleaseSdHolds(); }

namespace plus_hal {
void EarlyWakeGate() {
    const auto cause = esp_sleep_get_wakeup_cause();
    if (s_deep_sleep_magic != kDeepSleepMagic || cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        s_deep_sleep_magic = 0;
        return;
    }
    HoldFrontlightDark();
    // EXT1 owns GPIO2 as RTC IO. Release just that pad before the eventual
    // normal HAL init; the sleep picture and EPD/light holds remain intact.
    rtc_gpio_deinit(IO_EXPANDER_INT_GPIO);
    if (metalio_i2c1_init(I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK)
        EnterHardwareDeepSleep();
    WakeHoldGate hold;
    for (;;) {
        bool down = false;
        if (!ReadSleepKey(&down) || !down) {
            ESP_LOGI(kTag, "DEEP_WAKE rejected: short/released/non-power wake");
            EnterHardwareDeepSleep();
        }
        if (hold.Sample(down, esp_timer_get_time())) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_deep_sleep_magic = 0;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    rtc_gpio_deinit(IO_EXPANDER_INT_GPIO);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    // The normal board init releases EPD/FL holds, then initializes minimum
    // PWM. The runtime PowerKeyState ignores this still-held gesture until its
    // release, so it cannot immediately lock/sleep again.
    ESP_LOGI(kTag, "DEEP_WAKE accepted: continuous power hold=5000ms");
}

void SetLockscreenStyle(LockscreenStyle style) {
    if (!s_frame_gate) {
        s_lockscreen_style.store(style, std::memory_order_release);
        return;
    }
    xSemaphoreTake(s_frame_gate, portMAX_DELAY);
    const auto before = s_lockscreen_style.exchange(style, std::memory_order_acq_rel);
    const bool redraw = before != style && s_standby.load() && !s_powered_down.load();
    if (redraw) s_control_requested.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(s_frame_gate);
    if (redraw && s_display_task) xTaskNotifyGive(s_display_task);
}

bool Init(std::string* error) {
    if (error) error->clear();
    if (s_ready.load()) return true;
    if (s_init_attempted) return Fail(error, "previous HAL initialization failed; restart required");
    s_init_attempted = true;
    if (!plus_assets::Validate(app_assets::Lock, size_t(app_lock_end - app_lock_start)) ||
        !plus_assets::Validate(app_assets::Deep, size_t(app_deep_end - app_deep_start)))
        return Fail(error, "poster descriptor/format/linked-size mismatch");
    if (!esp_psram_is_initialized()) return Fail(error, "PSRAM initialization failed");
    s_frame_gate = xSemaphoreCreateMutex();
    s_panel_gate = xSemaphoreCreateMutex();
    s_aux_gate = xSemaphoreCreateMutex();
    s_init_done = xSemaphoreCreateBinary();
    s_motor_queue = xQueueCreate(32, sizeof(MotorRequest));
    if (!s_frame_gate || !s_panel_gate || !s_aux_gate || !s_init_done || !s_motor_queue)
        return Fail(error, "HAL synchronization allocation failed");
    s_pending = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, kGrayBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_pending) return Fail(error, "owned mono frame allocation failed");
    std::memset(s_pending, 0xff, kGrayBytes);
    s_display_task = xTaskCreateStatic(DisplayTask, "plus_display", sizeof(s_display_stack),
                                     nullptr, 4, s_display_stack, &s_display_tcb);
    if (!s_display_task) return Fail(error, "display worker creation failed");
    xSemaphoreTake(s_init_done, portMAX_DELAY);
    if (!s_panel_initialized) return Fail(error, s_panel_init_error.c_str());
    InitCharging();
    if (!InitFrontlight(error)) return false;
    epd_board_metalio_eink4_plus_motor_set(false);
    MountTf();
    metalio_touch_set_sample_cb(TouchSample);
    if (!metalio_touch_init()) return Fail(error, "FT6336U initialization failed");
    s_motor_task = xTaskCreateStatic(MotorTask, "plus_motor", sizeof(s_motor_stack),
                                   nullptr, 5, s_motor_stack, &s_motor_tcb);
    if (!s_motor_task) return Fail(error, "motor worker creation failed");
    s_power_task = xTaskCreateStatic(PowerTask, "plus_power", sizeof(s_power_stack),
                                   nullptr, 5, s_power_stack, &s_power_tcb);
    if (!s_power_task) return Fail(error, "power worker creation failed");
    gpio_config_t ai{};
    ai.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    ai.mode = GPIO_MODE_INPUT;
    ai.pull_up_en = GPIO_PULLUP_ENABLE;
    ai.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ai.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&ai) != ESP_OK) return Fail(error, "AI key GPIO61 input initialization failed");
    s_ready.store(true, std::memory_order_release);
    InvalidateTouches();
    ESP_LOGI(kTag, "ready native=1216x684 logical=684x1216 TF=%d VCOM=%dmV",
             int(s_tf_mounted.load()), EPD_VCOM_MV);
    return true;
}

bool QueueMono(const uint8_t* frame, int width, int height, bool full,
               uint32_t revision, std::string* error) {
    if (error) error->clear();
    if (!frame || !SupportedSize(width, height))
        return Fail(error, "expected MSB/1=black 800x480 or 1216x684 mono frame");
    if (!s_ready.load() || s_powered_down.load()) return Fail(error, "display not ready");
    if (xSemaphoreTake(s_frame_gate, 0) != pdTRUE) {
        if (error) *error = "display admission busy";
        return false;
    }
    if (s_pause_requested.load() || s_powered_down.load() || s_standby.load() || s_busy.load() ||
        s_control_busy.load() || s_control_requested.load() != s_control_handled.load()) {
        xSemaphoreGive(s_frame_gate);
        if (error) *error = "display queue busy, in standby, or paused for Flash";
        return false;
    }
    s_busy.store(true, std::memory_order_release);
    const size_t bytes = size_t(width) * height / 8;
    std::memcpy(s_pending, frame, bytes);
    s_pending_width = width; s_pending_height = height;
    s_pending_revision = revision; s_pending_full = full; s_pending_is_gray = false;
    // Complete private snapshot is published before the worker notification.
    xTaskNotifyGive(s_display_task);
    xSemaphoreGive(s_frame_gate);
    return true;
}

bool QueueGray4(const uint8_t* frame, bool full, uint32_t revision, std::string* error) {
    return QueueTestFrame(frame,full,revision,DisplayMode::Auto,error);
}
bool QueueTestFrame(const uint8_t* frame, bool full, uint32_t revision, DisplayMode mode,std::string* error) {
    if(mode==DisplayMode::MonoFast&&frame){
        for(size_t i=0;i<kGrayBytes;++i)
            if((frame[i]&15u)!=0&&(frame[i]&15u)!=15u)return Fail(error,"MonoFast received gray pixels");
            else if((frame[i]>>4)!=0&&(frame[i]>>4)!=15)return Fail(error,"MonoFast received gray pixels");
    }
    if(error) error->clear();
    if(!frame || !s_ready.load() || s_powered_down.load()) return Fail(error,"display not ready");
    if(xSemaphoreTake(s_frame_gate,0)!=pdTRUE) return false;
    if(s_pause_requested.load() || s_powered_down.load() || ScreenInputBlocked() || s_busy.load()) {
        xSemaphoreGive(s_frame_gate);return false;
    }
    s_busy.store(true,std::memory_order_release);
    std::memcpy(s_pending,frame,kGrayBytes);s_pending_width=kWidth;s_pending_height=kHeight;
    s_pending_is_gray=true;s_pending_full=full;s_pending_revision=revision;s_pending_mode=mode;
    xTaskNotifyGive(s_display_task);xSemaphoreGive(s_frame_gate);return true;
}

bool DisplayBusy() {
    return s_busy.load(std::memory_order_acquire) || s_control_busy.load(std::memory_order_acquire) ||
           s_control_requested.load(std::memory_order_acquire) !=
           s_control_handled.load(std::memory_order_acquire);
}
bool DisplayHealthy() {
    return s_ready.load() && s_healthy.load() && s_app_healthy.load() && !s_powered_down.load();
}
uint32_t DisplayedRevision() { return s_displayed_revision.load(std::memory_order_acquire); }

void InvalidateTouches() {
    portENTER_CRITICAL(&s_touch_mux);
    s_touch_queue.Invalidate();
    ClearPointerLocked();
    portEXIT_CRITICAL(&s_touch_mux);
}

bool PollTouch(int* x, int* y) {
    if (!x || !y || !s_ready.load() || s_pause_requested.load() || s_powered_down.load() ||
        ScreenInputBlocked() || !SelectInputConsumer(1)) return false;
    TouchEdge point{};
    portENTER_CRITICAL(&s_touch_mux);
    const bool valid = !s_pause_requested.load() && !s_powered_down.load() &&
                       !ScreenInputBlocked() && s_touch_queue.Pop(&point, esp_timer_get_time());
    portEXIT_CRITICAL(&s_touch_mux);
    if (valid) { *x = point.x; *y = point.y; }
    return valid;
}

std::string InputDiagnostics() {
    const auto report = metalio_touch_stats();
    portENTER_CRITICAL(&s_touch_mux);
    const auto fifo = s_touch_queue.Stats();
    const unsigned pending = unsigned(s_touch_queue.Size());
    const uint32_t epoch = s_touch_queue.Epoch();
    const bool waiting = s_touch_queue.WaitingRelease();
    portEXIT_CRITICAL(&s_touch_mux);
    char json[1536];
    const int n = std::snprintf(json, sizeof(json),
        "{\"report\":{\"samples\":%lu,\"errors\":%lu,\"zero\":%lu,\"down\":%lu,\"up\":%lu,"
        "\"contact\":%lu,\"multi\":%lu,\"duplicate_down\":%lu,\"recovered\":%lu,"
        "\"explicit_release\":%lu,\"id_change\":%lu,\"invalid\":%lu,\"suppressed\":%lu,"
        "\"max_gap_us\":%lu,\"max_i2c_wait_us\":%lu,\"max_i2c_read_us\":%lu},"
        "\"fifo\":{\"presses\":%lu,\"releases\":%lu,\"enqueued\":%lu,\"delivered\":%lu,"
        "\"overflow\":%lu,\"discarded\":%lu,\"invalidations\":%lu,\"blocked_samples\":%lu,"
        "\"outside_samples\":%lu,\"pending\":%u,\"epoch\":%lu,\"waiting_release\":%s,\"max_delivery_us\":%lu},"
        "\"motor\":{\"requested\":%lu,\"queued\":%lu,\"started\":%lu,\"finished\":%lu,"
        "\"dropped\":%lu,\"pending\":%u,\"max_delay_us\":%lu},\"samples_during_refresh\":%lu}",
        (unsigned long)report.samples, (unsigned long)report.read_errors, (unsigned long)report.raw_zero,
        (unsigned long)report.raw_down, (unsigned long)report.raw_up, (unsigned long)report.raw_contact,
        (unsigned long)report.raw_multi, (unsigned long)report.duplicate_down,
        (unsigned long)report.recovered_edges, (unsigned long)report.explicit_releases,
        (unsigned long)report.id_changes, (unsigned long)report.invalid_reports,
        (unsigned long)report.boundary_suppressed, (unsigned long)report.max_valid_gap_us,
        (unsigned long)report.max_i2c_wait_us, (unsigned long)report.max_i2c_read_us,
        (unsigned long)fifo.presses, (unsigned long)fifo.releases, (unsigned long)fifo.enqueued,
        (unsigned long)fifo.delivered, (unsigned long)fifo.overflow, (unsigned long)fifo.discarded,
        (unsigned long)fifo.invalidations, (unsigned long)fifo.blocked_samples,
        (unsigned long)fifo.outside_samples, pending, (unsigned long)epoch,
        waiting ? "true" : "false", (unsigned long)fifo.max_delivery_us,
        (unsigned long)s_motor_requested.load(), (unsigned long)s_motor_queued.load(),
        (unsigned long)s_motor_started.load(), (unsigned long)s_motor_finished.load(),
        (unsigned long)s_motor_dropped.load(),
        s_motor_queue ? unsigned(uxQueueMessagesWaiting(s_motor_queue)) : 0,
        (unsigned long)s_motor_max_delay_us.load(), (unsigned long)s_touch_samples_during_refresh.load());
    return n >= 0 && size_t(n) < sizeof(json) ? std::string(json, size_t(n)) :
        std::string("{\"error\":\"input diagnostics overflow\"}");
}

bool PollAiClick() {
    static plus_hal_detail::AiKeyState key;
    const bool blocked = !s_ready.load() || s_powered_down.load() ||
        s_pause_requested.load() || ScreenInputBlocked();
    const bool down = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    const bool click = key.Sample(down, blocked, s_standby_revision.load(), esp_timer_get_time());
    if (!blocked && down) RecordActivity(uint64_t(esp_timer_get_time()));
    if (click) ESP_LOGI(kTag, "AI_KEY_CLICK gpio=61");
    return click;
}

int PollVolumeDelta() {
    const auto epoch = s_volume_epoch.load(std::memory_order_acquire);
    const bool blocked = !s_ready.load() || s_powered_down.load() || s_pause_requested.load() ||
        s_power_off_event.load() || ScreenInputBlocked();
    const int steps = s_volume_delta.exchange(0, std::memory_order_acq_rel);
    if (blocked || s_volume_event_epoch.load(std::memory_order_acquire) != epoch ||
        s_volume_epoch.load(std::memory_order_acquire) != epoch) return 0;
    return steps;
}

bool SetAudioPower(bool enabled, std::string* error) {
    if (error) error->clear();
    if (!enabled) return Fail(error, "P14 is a shared display/audio rail; standalone power-off is forbidden");
    if (!s_ready.load() || s_powered_down.load() || s_pause_requested.load())
        return Fail(error, "audio power unavailable during shutdown or Flash operation");
    if (xSemaphoreTake(s_aux_gate, pdMS_TO_TICKS(1000)) != pdTRUE)
        return Fail(error, "audio power hardware gate timed out");
    const auto result = epd_board_metalio_eink4_plus_audio_power_set(enabled);
    xSemaphoreGive(s_aux_gate);
    return result == ESP_OK || Fail(error, "audio power rail write failed");
}

bool ResetAudioPower(std::string* error) {
    if (error) error->clear();
    if (!PauseForFlash(10000)) return Fail(error, "audio reset refused: shared hardware could not be drained");
    // Keep panel+aux gates, paused display admission and the paused touch feed.
    // The board helper takes its normal I2C lock for each latch write, so only
    // release our Flash-side I2C ownership. Never hold that bus across 3 s.
    if (s_flash_i2c_held) { metalio_i2c1_unlock(); s_flash_i2c_held = false; }
    InvalidateTouches();
    SetFrontlight(false);
    const auto reset_result = epd_board_metalio_eink4_plus_bt_power_reset();
    bool rail_high = reset_result == ESP_OK;
    if (!rail_high) {
        ESP_LOGE(kTag, "AUDIO_RESET failed=%s; restoring shared P14 high", esp_err_to_name(reset_result));
        for (unsigned attempt = 0; attempt < 3 && !rail_high; ++attempt) {
            rail_high = epd_board_metalio_eink4_plus_audio_power_set(true) == ESP_OK;
            if (!rail_high) vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // P14 also feeds TPS VOUT. A completed I2C high write is not the same as a
    // settled supply: prevent the display worker probing TPS during ramp-up.
    vTaskDelay(pdMS_TO_TICKS(700));
    if (!rail_high) {
        s_app_healthy.store(false, std::memory_order_release);
        SetFrontlight(false);
        ESP_LOGE(kTag, "AUDIO_RESET shared rail restore failed; retaining hardware pause until restart");
        return Fail(error, "shared display/audio power could not be restored; restart required");
    }
    InvalidateTouches();
    ResumeAfterFlash();
    if(!ScreenInputBlocked()) SetFrontlight(true);
    ESP_LOGI(kTag, "AUDIO_RESET settled=700ms result=%s", esp_err_to_name(reset_result));
    return reset_result == ESP_OK || Fail(error, "audio reset failed; shared power restored, please retry");
}

bool SetAudioAmplifier(bool enabled, std::string* error) {
    if (error) error->clear();
    if (!s_ready.load() || s_powered_down.load() || s_pause_requested.load())
        return Fail(error, "audio amplifier unavailable during shutdown or Flash operation");
    if (xSemaphoreTake(s_aux_gate, pdMS_TO_TICKS(1000)) != pdTRUE)
        return Fail(error, "audio amplifier hardware gate timed out");
    const auto result = epd_board_metalio_eink4_plus_audio_amplifier_set(enabled);
    xSemaphoreGive(s_aux_gate);
    return result == ESP_OK || Fail(error, "audio amplifier write failed");
}

bool ReadPointer(int* x, int* y) {
    if (!x || !y || !s_ready.load() || s_powered_down.load() || ScreenInputBlocked() || !SelectInputConsumer(2)) return false;
    bool pressed = false;
    portENTER_CRITICAL(&s_touch_mux);
    if (!s_touch_queue.WaitingRelease() && !s_pause_requested.load() && !ScreenInputBlocked()) {
        if (s_replay_press) { s_replay_press = false; pressed = true; }
        else if (s_pointer_pressed) { s_pointer_seen = true; pressed = true; }
        else if (s_replay_release) s_replay_release = false;
    }
    *x = s_pointer_x; *y = s_pointer_y;
    portEXIT_CRITICAL(&s_touch_mux);
    return pressed;
}

void Pulse() {
    QueueMotorRequest({s_motor_epoch.load(std::memory_order_acquire), esp_timer_get_time()});
}
bool TfMounted() { return s_tf_mounted.load() && !s_powered_down.load(); }

int BatteryPercent() {
    static int cached = -1;
    static int64_t last = -10000000;
    const int64_t now = esp_timer_get_time();
    if (!s_ready.load() || DisplayBusy() || ScreenInputBlocked() ||
        s_pause_requested.load() || s_powered_down.load() || now - last < 10000000)
        return cached;
    if (xSemaphoreTake(s_aux_gate, 0) != pdTRUE) return cached;
    if (!s_pause_requested.load() && !s_powered_down.load()) {
        bool charging = false, discharging = false;
        int level = 0;
        last = now;
        if (Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging))
            cached = std::clamp(level, 0, 100);
    }
    xSemaphoreGive(s_aux_gate);
    return cached;
}

bool PollPowerOff() {
    return s_ready.load() && !s_powered_down.load() &&
           s_power_off_event.exchange(false, std::memory_order_acq_rel);
}

bool IsStandby() { return ScreenInputBlocked(); }
uint32_t StandbyRevision() { return s_standby_revision.load(std::memory_order_acquire); }
uint64_t LastUserActivityUs() {
    portENTER_CRITICAL(&s_activity_mux);
    const uint64_t last = s_activity_us;
    portEXIT_CRITICAL(&s_activity_mux);
    return last;
}

bool PauseForFlash(uint32_t timeout_ms) {
    if (!s_ready.load() || s_powered_down.load()) return false;
    bool expected = false;
    if (!s_pause_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    CancelMotorRequests();
    InvalidateTouches();
    // A short Flash operation may begin/end between two power-task ticks. Its
    // epoch still forces release/rearm and prevents a stale cross-Flash hold.
    s_power_epoch.fetch_add(1, std::memory_order_acq_rel);
    s_volume_epoch.fetch_add(1, std::memory_order_acq_rel);
    s_volume_delta.store(0, std::memory_order_release);
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    bool panel = false, aux = false, touch_paused = false;
    // Fence a QueueMono that checked admission before we closed it.
    bool ok = TakeUntil(s_frame_gate, deadline);
    if (ok) xSemaphoreGive(s_frame_gate);
    // Wait only for accepted app work here. Control requests are deferred while
    // pause_requested and cannot be waited on; the panel gate drains any control
    // refresh that already started before admission was closed.
    while (ok && s_busy.load(std::memory_order_acquire)) {
        if (esp_timer_get_time() >= deadline) { ok = false; break; }
        vTaskDelay(1);
    }
    if (ok) { panel = TakeUntil(s_panel_gate, deadline); ok = panel; }
    if (ok) { aux = TakeUntil(s_aux_gate, deadline); ok = aux; }
    if (ok) {
        metalio_touch_set_feed_paused(true);
        touch_paused = true;
        // The feed rechecks its paused flag under this mutex. Holding it also
        // drains/quietens synchronous I2C IRQ state for the whole Flash write.
        const int64_t left = deadline - esp_timer_get_time();
        ok = metalio_i2c1_lock_for_flash(left > 0 ? int((left + 999) / 1000) : 0);
    }
    if (!ok) {
        if (touch_paused) metalio_touch_set_feed_paused(false);
        if (aux) xSemaphoreGive(s_aux_gate);
        if (panel) xSemaphoreGive(s_panel_gate);
        s_pause_requested.store(false, std::memory_order_release);
        if (s_display_task) xTaskNotifyGive(s_display_task);
        ESP_LOGE(kTag, "Flash quiesce timeout; no Flash write authorized by HAL");
        return false;
    }
    s_flash_i2c_held = true;
    s_flash_owner.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    ESP_LOGI(kTag, "FLASH_DISPLAY_QUIESCED frame=%lu", (unsigned long)DisplayedRevision());
    return true;
}

void ResumeAfterFlash() {
    if (s_flash_owner.load(std::memory_order_acquire) != xTaskGetCurrentTaskHandle()) return;
    if (s_powered_down.load()) return;
    if (s_flash_i2c_held) { metalio_i2c1_unlock(); s_flash_i2c_held = false; }
    metalio_touch_set_feed_paused(false);
    s_flash_owner.store(nullptr, std::memory_order_release);
    xSemaphoreGive(s_aux_gate);
    xSemaphoreGive(s_panel_gate);
    s_pause_requested.store(false, std::memory_order_release);
    if (s_display_task) xTaskNotifyGive(s_display_task);
    ESP_LOGI(kTag, "FLASH_DISPLAY_RESUMED");
}

bool PowerOff() {
    if (!s_ready.load() || s_powered_down.load()) return false;
    // The save caller may already own a Flash lease on this same task.
    if (s_flash_owner.load() != xTaskGetCurrentTaskHandle() && !PauseForFlash(30000)) {
        ESP_LOGE(kTag, "poweroff refused: hardware could not be drained");
        return false;
    }
    InvalidateTouches();
    // Retain panel/aux gates and paused admission. Board poweroff calls need
    // their normal I2C lock, so release only the Flash-side bus ownership.
    if (s_flash_i2c_held) { metalio_i2c1_unlock(); s_flash_i2c_held = false; }
    epd_board_metalio_eink4_plus_motor_set(false);
    auto restore_after_refusal = [] {
        // A failed sleep must not leave a fake sleep picture over a live app.
        // Gates are still owned here, so restoration finishes before touches
        // or business frames can be admitted again.
        const bool locked = s_standby.load(std::memory_order_acquire);
        EpdiyHighlevelState* state = MetalioEpd_Hl();
        if (locked) plus_assets::Decode(app_assets::Lock, app_lock_start, size_t(app_lock_end - app_lock_start), state->front_fb, kGrayBytes);
        else if (SupportedSize(s_pending_width, s_pending_height)) {
            if (s_pending_is_gray) std::memcpy(state->front_fb, s_pending, kGrayBytes);
            else ExpandMono(s_pending, s_pending_width, s_pending_height, state->front_fb);
        }
        else std::memset(state->front_fb, 0xFF, kGrayBytes);
        bool known = s_healthy.load();
        unsigned count = 0;
        const auto restored = PresentNative(state, false, known, count, "SLEEP_REFUSED_RESTORE", 0);
        // If the panel itself remains faulty, fail locked/dark rather than
        // exposing clickable controls under a misleading sleeping picture.
        if (restored != EPD_DRAW_SUCCESS) {
            s_standby.store(true, std::memory_order_release);
            s_standby_revision.fetch_add(1, std::memory_order_release);
            SetFrontlight(false);
        } else SetFrontlight(!locked);
        InvalidateTouches();
        ResumeAfterFlash();
    };
    // Save has finished. Own panel/aux gates and pause touch before submitting
    // this control picture; it must physically complete before power removal.
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    plus_assets::Decode(app_assets::Deep, app_deep_start, size_t(app_deep_end - app_deep_start), hl->front_fb, kGrayBytes);
    bool state_known = s_healthy.load();
    unsigned partials = 0;
    if (PresentNative(hl, false, state_known, partials, "DEEPSLEEP", 0) != EPD_DRAW_SUCCESS) {
        ESP_LOGE(kTag, "deep sleep refused: sleep picture not displayed");
        restore_after_refusal();
        return false;
    }
    SetFrontlight(false);
    if (s_tf_mounted.load() && s_card) {
        const esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        if (err == ESP_OK) {
            s_card = nullptr;
            s_tf_mounted.store(false);
            heap_caps_free(s_sd_dma); s_sd_dma = nullptr;
        } else {
            ESP_LOGE(kTag, "deep sleep refused: TF unmount: %s", esp_err_to_name(err));
            restore_after_refusal();
            return false;
        }
    }
    s_powered_down.store(true);
    s_healthy.store(false);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    s_sleep_tca_addr = pca9555_get_addr();
    // Finish the entering gesture before arming wake. Keeping the key held
    // cannot become the next 5-second wake gesture.
    ESP_LOGI(kTag, "DEEP_SLEEP poster ready/light off; waiting for power release");
    unsigned released_samples = 0;
    while (released_samples < 4) {
        bool down = true;
        released_samples = ReadSleepKey(&down) && !down ? released_samples + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    epd_poweroff();
    epd_board_metalio_eink4_plus_tps_enter_sleep();
    epd_board_metalio_eink4_plus_pa_set(false);
    epd_board_metalio_eink4_plus_bus_hold_low();
    epd_board_metalio_eink4_plus_bus_latch_for_sleep();
    epd_board_metalio_eink4_plus_sleep_cut_rails();
    epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
    if (!epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin())
        ESP_LOGE(kTag, "DEEP_SLEEP power-key input configuration failed");
    bool ignored = false;
    ReadSleepKey(&ignored);
    s_deep_sleep_magic = kDeepSleepMagic;
    EnterHardwareDeepSleep();
}
} // namespace plus_hal
