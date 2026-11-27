#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/storage/key_bindings.h"
#include "infra/tasks/task_create.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "boards/common/input/input.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/storage/sd_access.h"
#include "power_policy_priv.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

constexpr const char* TAG = "PowerPolicy";

static uint32_t s_standby_consumed = 0;

static bool DispatchStandbyBinding(BindingKey key, bool long_press) {
    const BindingAction action = KeyBindingsStandbyAction(key, long_press);
    if (action == BindingAction::None) {
        return false;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    if (action == BindingAction::Shutdown) {
        PowerPolicyPrepareStandbyWakeExitLocked();
        PowerPolicyBeginPowerOffLocked();
    } else {
        PowerPolicyWakeTouchLocked();
    }
    xSemaphoreGive(power_policy_mu);
    if (action == BindingAction::Wake) {
        PowerPolicyRequestDismissStandbyOverlay();
    }
    return true;
}

static bool PollStandbyPhysicalKeys() {
    if (StandbyPresentation::HasWakeRequest()) {
        PowerPolicyRequestDismissStandbyOverlay();
        return true;
    }
    const bool power_long = InputTakeShutdownRequest();
    const bool power_short = InputTakePowerClick();
    const uint32_t power_bit = 1u << static_cast<unsigned>(BindingKey::Power);
    if ((power_long || power_short) && !(s_standby_consumed & power_bit)) {
        s_standby_consumed |= power_bit;
        if (DispatchStandbyBinding(BindingKey::Power, power_long)) return true;
    }
    const uint32_t latch = power_hw_take_key_wake_latch();
    for (unsigned key = 0; key < 4; ++key) {
        const auto binding = static_cast<BindingKey>(key);
        if (!KeyBindingsNeedsWake(binding)) continue;
        const auto input = static_cast<InputKey>(key);
        const uint32_t bit = 1u << key;
        const bool down = InputKeyIsDown(input);
        if (s_standby_consumed & bit) {
            if (!down) s_standby_consumed &= ~bit;
            continue;
        }
        if (!(latch & bit) && !down) continue;
        const int threshold = key == static_cast<unsigned>(BindingKey::Power) ? kPowerLongPressMs : kBootLongPressMs;
        const int64_t started = esp_timer_get_time();
        while (InputKeyIsDown(input) && esp_timer_get_time() - started < threshold * 1000LL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        const bool long_press = esp_timer_get_time() - started >= threshold * 1000LL;
        s_standby_consumed |= bit;
        if (key == static_cast<unsigned>(BindingKey::Power)) {
            InputTakePowerClick();
            InputTakeShutdownRequest();
        }
        if (DispatchStandbyBinding(binding, long_press)) return true;
    }
    return false;
}

static bool HandleGpioWakeFromLightSleep() {
    return PollStandbyPhysicalKeys();
}

static bool DelayMsWithStandbyKeyPoll(uint32_t delay_ms) {
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

static bool AbortStandbyWait(const char* stage, int64_t deadline) {
    if (PollStandbyPhysicalKeys()) {
        return true;
    }
    if (esp_timer_get_time() < deadline) {
        return false;
    }
    ESP_LOGW(TAG, "operation=standby stage=%s result=timeout; sleep cancelled, power retained", stage);
    PowerPolicyRequestDismissStandbyOverlay();
    return true;
}

// 壁纸解码/经典页 flush 必须完成；超时只诊断，不强制卸卡。
static bool WaitStandbyPaintReady() {
    constexpr uint32_t kMinMs = 80;
    constexpr uint32_t kStepMs = 50;
    uint32_t elapsed_ms = 0;
    const int64_t deadline = esp_timer_get_time() + 30000000;
    ESP_LOGI(TAG, "LP phase wait_paint");
    while (StandbyPresentation::IsActive()) {
        if (AbortStandbyWait("paint", deadline)) {
            ESP_LOGI(TAG, "LP phase wait_paint: cancelled");
            return true;
        }
        if (elapsed_ms >= kMinMs && StandbyPresentation::IsPaintReady()) {
            ESP_LOGI(TAG, "LP phase wait_paint: ready after %u ms", static_cast<unsigned>(elapsed_ms));
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

static bool DrainSdAndBootWork() {
    // 全部前置条件归还后才停放SD；超时只取消入睡，不越过在途事务断电。
    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (const char* stage = PowerLifecyclePendingStandbyWork()) {
        if (AbortStandbyWait(stage, deadline)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (power_hw_sd_park() != ESP_OK) {
        if (AbortStandbyWait("sd_park", deadline)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return PollStandbyPhysicalKeys();
}

static int64_t StandbyUiRemainUsLocked() {
    const int64_t elapsed = power_policy_standby_elapsed_us;
    int64_t total = elapsed;
    if (power_policy_standby_awake_mark_us != 0) {
        total += esp_timer_get_time() - power_policy_standby_awake_mark_us;
    }
    if (total >= PowerPolicyStandbyToShutdownUs()) {
        return 0;
    }
    return PowerPolicyStandbyToShutdownUs() - total;
}

static void CloseStandbyAwakeSegmentLocked() {
    if (power_policy_standby_awake_mark_us == 0) {
        return;
    }
    power_policy_standby_elapsed_us += esp_timer_get_time() - power_policy_standby_awake_mark_us;
    power_policy_standby_awake_mark_us = 0;
}

static void OpenStandbyAwakeSegmentLocked() {
    power_policy_standby_awake_mark_us = esp_timer_get_time();
}

static bool IsKeyWakeCause(esp_sleep_wakeup_cause_t cause) {
    return cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1;
}

static bool RunLightSleepSlice() {
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (power_hw_standby_rails_park() != ESP_OK) {
        if (AbortStandbyWait("rail_park", deadline)) {
            return true;
        }
        ESP_LOGW(TAG, "standby rail cut failed; retry before sleep");
        if (DelayMsWithStandbyKeyPoll(500)) {
            return true;
        }
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const int64_t remain_us = StandbyUiRemainUsLocked();
    xSemaphoreGive(power_policy_mu);

    uint64_t slice = static_cast<uint64_t>(remain_us);
    if (slice > kPowerPolicyLightSleepSliceUs) {
        slice = kPowerPolicyLightSleepSliceUs;
    }
    if (slice < 1000) {
        slice = 1000;
    }

    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    CloseStandbyAwakeSegmentLocked();
    xSemaphoreGive(power_policy_mu);

    ESP_LOGI(TAG, "LP phase pre_sleep max_us=%llu", static_cast<unsigned long long>(slice));
    const esp_err_t sleep_result = power_hw_light_sleep_once(slice);
    ESP_LOGI(TAG, "LP phase post_sleep");
    if (sleep_result != ESP_OK) {
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(power_policy_mu);
        vTaskDelay(pdMS_TO_TICKS(20));
        return PollStandbyPhysicalKeys();
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (IsKeyWakeCause(cause)) {
        ESP_LOGI(TAG, "key wake cause=%d", static_cast<int>(cause));
        // GPIO2 为共享中断，不能仅凭 EXT1 原因合成电源键短按。
        if (HandleGpioWakeFromLightSleep()) {
            return true;
        }
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(power_policy_mu);
        return false;
    }
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        power_policy_standby_elapsed_us += static_cast<int64_t>(slice);
        OpenStandbyAwakeSegmentLocked();
        xSemaphoreGive(power_policy_mu);
        return false;
    }
    ESP_LOGW(TAG, "light sleep wake cause=%d, skip slice", static_cast<int>(cause));
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    OpenStandbyAwakeSegmentLocked();
    xSemaphoreGive(power_policy_mu);
    vTaskDelay(pdMS_TO_TICKS(50));
    return false;
}

static bool RunDelaySlice() {
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const int64_t remain_us = StandbyUiRemainUsLocked();
    const int64_t elapsed_s =
        (power_policy_standby_elapsed_us +
         (power_policy_standby_awake_mark_us != 0 ? esp_timer_get_time() - power_policy_standby_awake_mark_us : 0)) /
        1000000;
    xSemaphoreGive(power_policy_mu);

    uint64_t slice = static_cast<uint64_t>(remain_us);
    if (slice > kPowerPolicyLightSleepSliceUs) {
        slice = kPowerPolicyLightSleepSliceUs;
    }
    if (slice < 1000) {
        slice = 1000;
    }
    const uint32_t delay_ms = static_cast<uint32_t>(slice / 1000 > 0 ? slice / 1000 : 1);
    ESP_LOGI(TAG, "LP slice delay %lu ms (light sleep off, elapsed %ld s)", static_cast<unsigned long>(delay_ms),
             static_cast<long>(elapsed_s));
    return DelayMsWithStandbyKeyPoll(delay_ms);
}

static bool StandbyLpShouldContinue() {
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    if (power_policy_shutdown_requested) {
        xSemaphoreGive(power_policy_mu);
        return false;
    }
    const bool ok = power_policy_mode == PowerPolicyMode::StandbyUi && power_policy_board != nullptr &&
                    StandbyPresentation::IsActive() && !power_policy_standby_wake_exit;
    if (!ok) {
        PowerPolicyWakeTouchLocked();
        power_policy_standby_ui_requested = false;
        PowerPolicyClearStandbyElapsedLocked();
    }
    xSemaphoreGive(power_policy_mu);
    return ok;
}

static bool RunStandbyLpMainLoop() {
    for (;;) {
        if (!StandbyLpShouldContinue()) {
            return false;
        }
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        if (PowerPolicyStandbyUiLongEnoughLocked()) {
            PowerPolicyRequestShutdownLocked();
            xSemaphoreGive(power_policy_mu);
            return false;
        }
        xSemaphoreGive(power_policy_mu);

        if (PollStandbyPhysicalKeys()) {
            return true;
        }

        const bool key_done = kPowerPolicyStandbyLpUseLightSleep ? RunLightSleepSlice() : RunDelaySlice();
        if (key_done) {
            return true;
        }
    }
}

static void StandbyLpTask(void* /*arg*/) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "standby LP session start");

        int waited_ms = 0;
        while (!StandbyPresentation::IsActive() && waited_ms < 8000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited_ms += 50;
        }
        if (!StandbyPresentation::IsActive()) {
            ESP_LOGW(TAG, "standby UI not active, LP session abort");
            xSemaphoreTake(power_policy_mu, portMAX_DELAY);
            power_policy_standby_ui_requested = false;
            PowerPolicyClearStandbyElapsedLocked();
            power_policy_lp_session_active = false;
            PowerPolicyReevaluateLocked();
            xSemaphoreGive(power_policy_mu);
            PowerPolicyFlushCpuFreqOutsideLock();
            continue;
        }

        s_standby_consumed = 0;
        for (unsigned key = 0; key < 4; ++key) {
            if (InputKeyIsDown(static_cast<InputKey>(key))) s_standby_consumed |= 1u << key;
        }

        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        PowerPolicyEnsureTouchSleepLocked();
        xSemaphoreGive(power_policy_mu);

        ESP_LOGI(TAG, "LP phase touch_slp done (asleep=%d)", PowerPolicy::GetInstance().IsTouchAsleep() ? 1 : 0);
        bool key_in_settle = WaitStandbyPaintReady();
        if (!key_in_settle) {
            key_in_settle = DrainSdAndBootWork();
        }
        if (!key_in_settle) {
            ESP_LOGI(TAG, "LP phase epd_park");
            const int64_t park_deadline = esp_timer_get_time() + 10000000;
            while (!StandbyPresentation::EnterEpdSleep()) {
                if (AbortStandbyWait("display_park", park_deadline)) {
                    key_in_settle = true;
                    break;
                }
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

        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        const bool wake_exit = power_policy_standby_wake_exit;
        const bool shutting_down = power_policy_shutdown_requested;
        power_policy_lp_session_active = false;
        xSemaphoreGive(power_policy_mu);
        unsigned restore_attempts = 0;
        while (!shutting_down && power_hw_standby_rails_restore() != ESP_OK) {
            if (restore_attempts++ % 20 == 0) {
                ESP_LOGE(TAG, "standby exit: rail restore failed; power retained, retrying");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (wake_exit && !shutting_down) {
            // Also covers wake during draining, before light_sleep_once was called.
            power_hw_sd_restore();
            PowerLifecycleResumeCellular();
            InputTakePowerClick();
            // Do not depend on MainEventLoop, which may still be waiting for a
            // network request. Retry the LVGL queue with the wake request intact.
            while (StandbyPresentation::IsActive() && !ScreenLvAsyncUrgent([](void*) { StandbyPresentation::FinishWake(); })) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        ESP_LOGI(TAG, "standby LP session end");
    }
}

void PowerPolicyEnsureTouchSleepLocked() {
    if (power_policy_board == nullptr || power_policy_touch_asleep) {
        return;
    }
    if (power_policy_board->TouchEnterSleep() == ESP_OK) {
        power_policy_touch_asleep = true;
        ESP_LOGI(TAG, "touch sleep for standby LP");
    }
}

void PowerPolicyClearStandbyElapsedLocked() {
    power_policy_standby_elapsed_us = 0;
    power_policy_standby_awake_mark_us = 0;
}

void PowerPolicyResetStandbyElapsedLocked() {
    power_policy_standby_elapsed_us = 0;
    power_policy_standby_awake_mark_us = esp_timer_get_time();
}

bool PowerPolicyStandbyUiLongEnoughLocked() {
    int64_t elapsed = power_policy_standby_elapsed_us;
    if (power_policy_standby_awake_mark_us != 0) {
        elapsed += esp_timer_get_time() - power_policy_standby_awake_mark_us;
    }
    return elapsed >= PowerPolicyStandbyToShutdownUs();
}

void PowerPolicyWakeTouchLocked() {
    if (power_policy_board == nullptr || !power_policy_touch_asleep) {
        return;
    }
    if (power_policy_board->TouchWakeByReset() == ESP_OK) {
        power_policy_touch_asleep = false;
    }
}

void PowerPolicyPrepareStandbyWakeExitLocked() {
    power_policy_standby_ui_requested = false;
    power_policy_standby_wake_exit = true;
    PowerPolicyClearStandbyElapsedLocked();
}

void PowerPolicyRequestDismissStandbyOverlay() {
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    PowerPolicyPrepareStandbyWakeExitLocked();
    power_policy_last_user_activity_us = esp_timer_get_time();
    xSemaphoreGive(power_policy_mu);
    if (!StandbyPresentation::HasWakeRequest()) {
        StandbyPresentation::RequestWake();
    }
}

void PowerPolicyRequestStandbyUiAsync() {
    if (power_policy_standby_ui_requested) {
        return;
    }
    power_policy_standby_ui_requested = true;
    power_policy_standby_show_pending = true;
}

void PowerPolicyRequestShutdownLocked() {
    if (power_policy_shutdown_requested) {
        return;
    }
    const int64_t elapsed_s =
        (power_policy_standby_elapsed_us +
         (power_policy_standby_awake_mark_us != 0 ? esp_timer_get_time() - power_policy_standby_awake_mark_us : 0)) /
        1000000;
    ESP_LOGW(TAG, "standby LP idle elapsed=%ld s (limit %d s) -> power off", static_cast<long>(elapsed_s),
             power_policy_standby_to_shutdown_sec);
    PowerPolicyBeginPowerOffLocked();
}

void PowerPolicyArmStandbyLpTaskLocked() {
    if (power_policy_lp_task == nullptr || power_policy_lp_session_active || power_policy_shutdown_requested ||
        power_policy_standby_wake_exit) {
        return;
    }
    power_policy_lp_session_active = true;
    xTaskNotifyGive(power_policy_lp_task);
}

void PowerPolicyStartStandbyLpTask() {
    if (power_policy_lp_task != nullptr) {
        return;
    }
    BaseType_t ok = FirmwareCreateTask(StandbyLpTask, "standby_lp", 4096, nullptr, tskIDLE_PRIORITY + 3,
                                                    &power_policy_lp_task, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        power_policy_lp_task = nullptr;
        ESP_LOGE(TAG, "create standby LP task failed");
    }
}
