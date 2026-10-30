#include "power_policy_priv.h"

#include "power_hw.h"
#include "standby_screen/standby_screen.h"
#include "screen_common.h"
#include "metalio_keys.h"
#include "config.h"
#include "application.h"
#include "sd_access.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "PowerPolicy";

constexpr int kKeyBoot = 0;
constexpr int kKeyPower = 3;

int MeasureKeyHoldMs(int key_index)
{
    const int64_t started = esp_timer_get_time();
    int held_ms = 0;
    for (;;) {
        if (key_index == kKeyPower && MetalioKeys_TakeShutdownPulse()) {
            return kPowerLongPressMs;
        }
        if (!MetalioKeys_IsDown(key_index)) break;
        held_ms = static_cast<int>((esp_timer_get_time() - started) / 1000);
        if (key_index == kKeyBoot && held_ms >= kBootLongPressMs) {
            break;
        }
        if (key_index == kKeyPower && held_ms >= kPowerLongPressMs) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return held_ms;
}

/** @return true 结束本轮 LP（出待机）；false 继续浅睡 */
bool RunStandbyBootKeyGesture(int held_ms)
{
    if (held_ms >= kBootLongPressMs) {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        pwr::WakeTouchLocked();
        pwr::ResetStandbyElapsedLocked();
        pwr::last_user_activity_us = esp_timer_get_time();
        pwr::PrepareStandbyWakeExitLocked();
        xSemaphoreGive(pwr::mu);
        ESP_LOGI(TAG, "BOOT standby long %d ms -> assistant", held_ms);
        StandbyScreen::HandleBootLongPress();
        return true;
    }
    ESP_LOGI(TAG, "BOOT standby short %d ms -> stay LP", held_ms);
    return false;
}

void RunStandbyPowerKeyGesture(int held_ms)
{
    const bool is_long = held_ms >= kPowerLongPressMs;
    if (is_long) {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        pwr::PrepareStandbyWakeExitLocked();
        ESP_LOGW(TAG, "POWER standby long %d ms -> power off", held_ms);
        pwr::BeginPowerOffLocked();
        xSemaphoreGive(pwr::mu);
    } else {
        ESP_LOGI(TAG, "POWER standby short %d ms -> dismiss", held_ms);
        pwr::RequestDismissStandbyOverlay();
    }
}

bool DispatchStandbyPowerKey(int held_ms)
{
    if (held_ms < kPowerLongPressMs) {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        pwr::WakeTouchLocked();
        pwr::ResetStandbyElapsedLocked();
        pwr::last_user_activity_us = esp_timer_get_time();
        xSemaphoreGive(pwr::mu);
    }
    RunStandbyPowerKeyGesture(held_ms);
    return true;
}

bool PollStandbyPhysicalKeys()
{
    if (MetalioKeys_TakeShutdownPulse()) {
        return DispatchStandbyPowerKey(kPowerLongPressMs);
    }
    if (StandbyScreen::HasWakeRequest()) {
        pwr::RequestDismissStandbyOverlay();
        return true;
    }
    if (MetalioKeys_TakePowerClick()) {
        return DispatchStandbyPowerKey(0);
    }
    const bool boot_held = MetalioKeys_IsDown(kKeyBoot);
    const bool power_held = MetalioKeys_IsDown(kKeyPower);
    if (!boot_held && !power_held) {
        return false;
    }
    ESP_LOGI(TAG, "standby key boot=%d power=%d", boot_held ? 1 : 0, power_held ? 1 : 0);
    if (boot_held) {
        return RunStandbyBootKeyGesture(MeasureKeyHoldMs(kKeyBoot));
    }
    const int held = MeasureKeyHoldMs(kKeyPower);
    if (held >= kPowerLongPressMs) return DispatchStandbyPowerKey(held);
    // Let the sampling task publish the release. Never synthesize a second
    // click from the raw level: that would re-enter standby after dismissal.
    for (int i = 0; i < 10; ++i) {
        if (MetalioKeys_TakePowerClick()) return DispatchStandbyPowerKey(0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/**
 * 浅睡 EXT1 醒：唤醒源仅 P17→GPIO2。短按常在读脚前已松开，仍按 POWER 短按 dismiss。
 * @return true 结束本轮 LP；false 继续浅睡（本路径对 POWER 唤醒应恒为 true）
 */
bool HandleGpioWakeFromLightSleep()
{
    if (MetalioKeys_TakeShutdownPulse()) {
        return DispatchStandbyPowerKey(kPowerLongPressMs);
    }
    bool latch_boot = false;
    bool latch_power = false;
    power_hw_gpio_wake_latch_get(&latch_boot, &latch_power);
    (void)latch_boot;
    bool saw_power = latch_power || MetalioKeys_IsDown(kKeyPower);

    constexpr int kGraceMs = 300;
    constexpr int kStepMs = 10;
    for (int waited = 0; !saw_power && waited < kGraceMs; waited += kStepMs) {
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
        saw_power = MetalioKeys_IsDown(kKeyPower);
    }

    ESP_LOGI(TAG, "EXT1 wake (POWER/P17) latch_power=%d", saw_power ? 1 : 0);

    if (saw_power) {
        const int held = MeasureKeyHoldMs(kKeyPower);
        return DispatchStandbyPowerKey(held);
    }
    // 脚已松开：仍是 POWER 唤醒事件 → 短按 dismiss（勿再 Poll BOOT）
    ESP_LOGI(TAG, "EXT1 wake POWER released -> short dismiss");
    return DispatchStandbyPowerKey(0);
}

bool DelayMsWithStandbyKeyPoll(uint32_t delay_ms)
{
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < delay_ms) {
        if (PollStandbyPhysicalKeys()) {
            return true;
        }
        const uint32_t step_ms = (delay_ms - elapsed_ms > 50) ? 50 : (delay_ms - elapsed_ms);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed_ms += step_ms;
    }
    return false;
}

/** 壁纸解码/经典页 flush 必须完成；超时只诊断，不强制卸卡。 */
bool WaitStandbyPaintReady()
{
    constexpr uint32_t kMinMs = 80;
    constexpr uint32_t kStepMs = 50;
    uint32_t elapsed_ms = 0;
    ESP_LOGI(TAG, "LP phase wait_paint (no forced SD timeout)");
    while (StandbyScreen::IsActive()) {
        if (PollStandbyPhysicalKeys()) {
            ESP_LOGI(TAG, "LP phase wait_paint: key -> skip park/sleep");
            return true;
        }
        if (elapsed_ms >= kMinMs && StandbyScreen::IsPaintReady()) {
            ESP_LOGI(TAG, "LP phase wait_paint: ready after %u ms",
                     static_cast<unsigned>(elapsed_ms));
            return false;
        }
        if (elapsed_ms != 0 && elapsed_ms % 2000 == 0) {
            ESP_LOGW(TAG, "waiting standby decode/paint; SD remains available to decoder");
        }
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
        elapsed_ms += kStepMs;
    }
    return true;
}

bool DrainSdAndBootWork()
{
    unsigned waited_ms = 0;
    while (!sd_access::IsIdle()) {
        if (PollStandbyPhysicalKeys()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
        if (waited_ms % 2000 == 0) {
            ESP_LOGI(TAG, "SD drain pending=%u waited=%u ms", sd_access::Pending(), waited_ms);
        }
    }
    if (PollStandbyPhysicalKeys()) return true;
    ESP_LOGI(TAG, "SD drained; stopping SD before waiting for boot checkpoint");
    while (power_hw_sd_park() != ESP_OK) {
        if (PollStandbyPhysicalKeys()) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    while (Application::GetInstance().IsBootWorkActive()) {
        if (PollStandbyPhysicalKeys()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 业务任务归还网络和音频后，才允许暂停 CPU 并切断共享外设电源。
    while (Application::GetInstance().HasNetworkAudioWork() ||
           PowerPolicy::GetInstance().HasForegroundNetworkWork() || pwr::HasPendingHardwareWork()) {
        if (PollStandbyPhysicalKeys()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return PollStandbyPhysicalKeys();
}

int64_t StandbyUiRemainUsLocked()
{
    if (pwr::standby_to_shutdown_sec <= 0) {
        return static_cast<int64_t>(pwr::kLightSleepSliceUs);
    }
    const int64_t elapsed = pwr::standby_elapsed_us;
    int64_t total = elapsed;
    if (pwr::standby_awake_mark_us != 0) {
        total += esp_timer_get_time() - pwr::standby_awake_mark_us;
    }
    if (total >= pwr::StandbyToShutdownUs()) {
        return 0;
    }
    return pwr::StandbyToShutdownUs() - total;
}

void CloseStandbyAwakeSegmentLocked()
{
    if (pwr::standby_awake_mark_us == 0) {
        return;
    }
    pwr::standby_elapsed_us += esp_timer_get_time() - pwr::standby_awake_mark_us;
    pwr::standby_awake_mark_us = 0;
}

void OpenStandbyAwakeSegmentLocked()
{
    pwr::standby_awake_mark_us = esp_timer_get_time();
}

bool IsKeyWakeCause(esp_sleep_wakeup_cause_t cause)
{
    return cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1;
}

bool RunLightSleepSlice()
{
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const int64_t remain_us = StandbyUiRemainUsLocked();
    xSemaphoreGive(pwr::mu);

    uint64_t slice = static_cast<uint64_t>(remain_us);
    if (slice > pwr::kLightSleepSliceUs) {
        slice = pwr::kLightSleepSliceUs;
    }
    if (slice < 1000) {
        slice = 1000;
    }

    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    CloseStandbyAwakeSegmentLocked();
    xSemaphoreGive(pwr::mu);

    ESP_LOGI(TAG, "LP phase pre_sleep max_us=%llu", static_cast<unsigned long long>(slice));
    const esp_err_t sleep_result = power_hw_light_sleep_once(slice);
    ESP_LOGI(TAG, "LP phase post_sleep");
    if (sleep_result != ESP_OK) {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(pwr::mu);
        vTaskDelay(pdMS_TO_TICKS(20));
        return PollStandbyPhysicalKeys();
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (IsKeyWakeCause(cause)) {
        ESP_LOGI(TAG, "key wake cause=%d", static_cast<int>(cause));
        // EXT1=GPIO2 仅 P17 可拉：一律按 POWER 处理（勿 Poll BOOT）
        if (HandleGpioWakeFromLightSleep()) {
            return true;
        }
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(pwr::mu);
        return false;
    }
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        pwr::standby_elapsed_us += static_cast<int64_t>(slice);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(pwr::mu);
        return false;
    }
    ESP_LOGW(TAG, "light sleep wake cause=%d, skip slice", static_cast<int>(cause));
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    OpenStandbyAwakeSegmentLocked();
    xSemaphoreGive(pwr::mu);
    vTaskDelay(pdMS_TO_TICKS(50));
    return false;
}

bool RunDelaySlice()
{
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const int64_t remain_us = StandbyUiRemainUsLocked();
    const int64_t elapsed_s =
        (pwr::standby_elapsed_us +
         (pwr::standby_awake_mark_us != 0 ? esp_timer_get_time() - pwr::standby_awake_mark_us : 0)) /
        1000000;
    xSemaphoreGive(pwr::mu);

    uint64_t slice = static_cast<uint64_t>(remain_us);
    if (slice > pwr::kLightSleepSliceUs) {
        slice = pwr::kLightSleepSliceUs;
    }
    if (slice < 1000) {
        slice = 1000;
    }
    const uint32_t delay_ms = static_cast<uint32_t>(slice / 1000 > 0 ? slice / 1000 : 1);
    ESP_LOGI(TAG, "LP slice delay %lu ms (light sleep off, elapsed %ld s)",
             static_cast<unsigned long>(delay_ms), static_cast<long>(elapsed_s));
    return DelayMsWithStandbyKeyPoll(delay_ms);
}

bool StandbyLpShouldContinue()
{
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    if (pwr::shutdown_requested) {
        xSemaphoreGive(pwr::mu);
        return false;
    }
    const bool ok =
        pwr::mode == pwr::Mode::StandbyUi && pwr::board != nullptr && StandbyScreen::IsActive() &&
        !pwr::standby_wake_exit;
    if (!ok) {
        pwr::WakeTouchLocked();
        pwr::standby_ui_requested = false;
        pwr::ClearStandbyElapsedLocked();
    }
    xSemaphoreGive(pwr::mu);
    return ok;
}

bool RunStandbyLpMainLoop()
{
    for (;;) {
        if (!StandbyLpShouldContinue()) {
            return false;
        }
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        if (pwr::StandbyUiLongEnoughLocked()) {
            pwr::RequestShutdownLocked();
            xSemaphoreGive(pwr::mu);
            return false;
        }
        xSemaphoreGive(pwr::mu);

        if (PollStandbyPhysicalKeys()) {
            return true;
        }

        const bool key_done =
            pwr::kStandbyLpUseLightSleep ? RunLightSleepSlice() : RunDelaySlice();
        if (key_done) {
            return true;
        }
    }
}

void StandbyLpTask(void* /*arg*/)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "standby LP session start");

        int waited_ms = 0;
        while (!StandbyScreen::IsActive() && waited_ms < 8000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited_ms += 50;
        }
        if (!StandbyScreen::IsActive()) {
            ESP_LOGW(TAG, "standby UI not active, LP session abort");
            xSemaphoreTake(pwr::mu, portMAX_DELAY);
            pwr::standby_ui_requested = false;
            pwr::ClearStandbyElapsedLocked();
            pwr::lp_session_active = false;
            pwr::ReevaluateLocked();
            xSemaphoreGive(pwr::mu);
            pwr::FlushCpuFreqOutsideLock();
            continue;
        }

        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        pwr::EnsureTouchSleepLocked();
        xSemaphoreGive(pwr::mu);

        ESP_LOGI(TAG, "LP phase touch_slp done (asleep=%d)",
                 PowerPolicy::GetInstance().IsTouchAsleep() ? 1 : 0);
        bool key_in_settle = WaitStandbyPaintReady();
        if (!key_in_settle) key_in_settle = DrainSdAndBootWork();
        if (!key_in_settle) {
            ESP_LOGI(TAG, "LP phase epd_park");
            while (!StandbyScreen::EnterEpdSleep()) {
                if (DelayMsWithStandbyKeyPoll(500)) {
                    key_in_settle = true;
                    break;
                }
            }
            if (!key_in_settle) {
                ESP_LOGI(TAG, "LP phase lp_loop");
                RunStandbyLpMainLoop();
            }
        } else {
            ESP_LOGI(TAG, "LP phase skip park/loop (key in settle)");
        }

        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        const bool wake_exit = pwr::standby_wake_exit;
        const bool shutting_down = pwr::shutdown_requested;
        pwr::lp_session_active = false;
        xSemaphoreGive(pwr::mu);
        if (wake_exit && !shutting_down) {
            // Also covers wake during draining, before light_sleep_once was called.
            power_hw_sd_restore();
            power_hw_cellular_resume();
            MetalioKeys_TakePowerClick();
            // Do not depend on MainEventLoop, which may still be waiting for a
            // network request. Retry the LVGL queue with the wake request intact.
            while (StandbyScreen::IsActive() &&
                   !ScreenLvAsyncUrgent([](void*) { StandbyScreen::FinishWake(); })) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        ESP_LOGI(TAG, "standby LP session end");
    }
}

}  // namespace

namespace pwr {

void EnsureTouchSleepLocked()
{
    if (board == nullptr || touch_asleep) {
        return;
    }
    if (board->TouchEnterSleep() == ESP_OK) {
        touch_asleep = true;
        ESP_LOGI(TAG, "touch sleep for standby LP");
    }
}

void ClearStandbyElapsedLocked()
{
    standby_elapsed_us = 0;
    standby_awake_mark_us = 0;
}

void ResetStandbyElapsedLocked()
{
    standby_elapsed_us = 0;
    standby_awake_mark_us = esp_timer_get_time();
}

bool StandbyUiLongEnoughLocked()
{
    if (standby_to_shutdown_sec <= 0) {
        return false;
    }
    int64_t elapsed = standby_elapsed_us;
    if (standby_awake_mark_us != 0) {
        elapsed += esp_timer_get_time() - standby_awake_mark_us;
    }
    return elapsed >= StandbyToShutdownUs();
}

void WakeTouchLocked()
{
    if (board == nullptr || !touch_asleep) {
        return;
    }
    if (board->TouchWakeByReset() == ESP_OK) {
        touch_asleep = false;
    }
}

void PrepareStandbyWakeExitLocked()
{
    standby_ui_requested = false;
    standby_wake_exit = true;
    ClearStandbyElapsedLocked();
}

void RequestDismissStandbyOverlay()
{
    xSemaphoreTake(mu, portMAX_DELAY);
    PrepareStandbyWakeExitLocked();
    last_user_activity_us = esp_timer_get_time();
    xSemaphoreGive(mu);
    if (!StandbyScreen::HasWakeRequest()) StandbyScreen::RequestWake();
}

void RequestStandbyUiAsync()
{
    if (standby_ui_requested) {
        return;
    }
    standby_ui_requested = true;
    standby_show_pending = true;
}

void RequestShutdownLocked()
{
    if (shutdown_requested) {
        return;
    }
    const int64_t elapsed_s =
        (standby_elapsed_us +
         (standby_awake_mark_us != 0 ? esp_timer_get_time() - standby_awake_mark_us : 0)) /
        1000000;
    ESP_LOGW(TAG, "standby LP idle elapsed=%ld s (limit %d s) -> power off",
             static_cast<long>(elapsed_s), standby_to_shutdown_sec);
    BeginPowerOffLocked();
}

void ArmStandbyLpTaskLocked()
{
    if (lp_task == nullptr || lp_session_active || shutdown_requested || standby_wake_exit) {
        return;
    }
    lp_session_active = true;
    xTaskNotifyGive(lp_task);
}

void StartStandbyLpTask()
{
    if (lp_task != nullptr) {
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        StandbyLpTask, "standby_lp", 4096, nullptr, tskIDLE_PRIORITY + 3, &lp_task, 0,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        lp_task = nullptr;
        ESP_LOGE(TAG, "create standby LP task failed");
    }
}

}  // namespace pwr
