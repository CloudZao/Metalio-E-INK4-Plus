#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <wifi_station.h>
#include "business/app_store/app_network.h"
#include "infra/tasks/task_create.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "business/network/wifi_setup.h"
#include "business/app_store/app_camera_preview.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "power_lifecycle.h"
#include "sysmgmt/power/standby_presentation.h"
#include "infra/network/wifi_radio.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "boards/common/board/board.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/board/nt26_board.h"
#include "boards/common/power/bq27220/bq27220_gauge.h"
#include "boards/common/power/cx25601n/cx25601n.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/storage/sd_access.h"
#include "boards/common/storage/sd_log_writer.h"
#include "config.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display.h"
#include "ui/common/drawing/font/ttf_convert.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr const char* TAG = "PowerLifecycle";
// 关机包含 LVGL 重绘、存储访问和显示收尾，按完整同步调用链预留栈。
static constexpr uint32_t kPowerOffStackBytes = 16 * 1024;
static constexpr uint32_t kPowerOffStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

static std::atomic<bool> s_power_off_started{false};
static std::atomic<bool> s_resuming{false};
static std::atomic<uint32_t> s_resume_sequence{0};
static std::atomic<uint32_t> s_resume_errors{0};

static Nt26Board* CellularBoard() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual && dual->GetNetworkType() == NetworkType::CELLULAR ? dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard())
                                                                   : nullptr;
}

static void PrepareNetworkPowerOff() {
    if (auto* cell = CellularBoard()) {
        cell->PreparePowerOff();
    }
}

static void NetworkPowerRestored(bool recover) {
    if (auto* cell = CellularBoard()) {
        cell->OnPowerRestored();
        if (recover) {
            cell->RecoverAfterPowerOnAsync();
        }
    }
}

static bool SleepCancelled() {
    return SystemState::GetInstance().IsStartupBusy() || AudioRuntime::GetInstance().HasWork() ||
           StandbyPresentation::HasWakeRequest();
}

static void NotifyNetworkChanged() {
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->UpdateStatusBar(true);
    }
}

static void PowerOffTask(void*) {
    TtfConvertCancel();
    const int64_t font_deadline = esp_timer_get_time() + 30000000;
    while ((TtfConvertBusy() || AppCameraPreviewBusy() || AppNetworkBusy()) && esp_timer_get_time() < font_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_err_t result = (TtfConvertBusy() || AppCameraPreviewBusy() || AppNetworkBusy()) ? ESP_ERR_TIMEOUT : power_hw_prepare_power_off();
    if (result == ESP_OK) {
        power_hw_pa_set(false);
        const auto sd = power_hw_sd_restore();
        if (sd != ESP_OK) {
            ESP_LOGW(TAG, "operation=power_off stage=sd_restore code=%d; default wallpaper", static_cast<int>(sd));
        }
        auto* display = LVAdapterDisplay::Instance();
        const int64_t deadline = esp_timer_get_time() + 10000000;
        bool painted = false;
        while (display && !(painted = display->ShowPoweredOffScreen()) && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!painted) {
            result = ESP_ERR_TIMEOUT;
        }
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "operation=power_off stage=prepare code=%d; power retained", static_cast<int>(result));
        unsigned restore_attempts = 0;
        while (power_hw_standby_rails_restore() != ESP_OK) {
            if (restore_attempts++ % 20 == 0) {
                ESP_LOGE(TAG, "operation=power_off stage=restore_rails result=retrying; input remains paused");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        const auto sd = power_hw_sd_restore();
        if (sd != ESP_OK) {
            ESP_LOGW(TAG, "operation=power_off stage=rollback_sd code=%d", static_cast<int>(sd));
        }
        PowerPolicy::GetInstance().OnPowerOffFailed();
        while (!ScreenLvAsyncUrgent([](void*) {
            UiRollbackPowerOff();
            s_power_off_started.store(false);
        })) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "operation=power_off stage=pulse");
    bool failed = false;
    for (;;) {
        const auto pulse = power_hw_power_off_pulse();
        if (pulse != ESP_OK && !failed) {
            ESP_LOGE(TAG, "operation=power_off stage=pulse code=%d", static_cast<int>(pulse));
        }
        failed = pulse != ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static std::atomic<bool> s_system_standby{false};

static void BatteryShutdownTask(void* arg) {
    const bool low_voltage = arg != nullptr;
    auto* display = LVAdapterDisplay::Instance();
    const bool shown = display != nullptr && display->ShowLowBatteryShutdownNotice(low_voltage);
    if (!shown) {
        ESP_LOGW(TAG, "Battery protect: notice unavailable; continue protective shutdown");
    }
    const esp_err_t err = cx25601n_enter_batfet_shutdown();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Battery protect: BATFET shutdown failed, err=%d; use power key", err);
    }
    PowerPolicy::GetInstance().RequestPowerOff();
    vTaskDeleteWithCaps(nullptr);
}

void PowerLifecycleInit() {
    const PowerHardwareCallbacks callbacks{PrepareNetworkPowerOff, NetworkPowerRestored, PowerLifecycleResumeCellular,
                                           SleepCancelled};
    power_hw_configure(&callbacks);
}

void PowerLifecycleResumeCellular() {
    if (auto* cell = CellularBoard()) {
        cell->RecoverAfterPowerOnAsync();
    }
}

esp_err_t PowerLifecyclePauseNetwork() {
    if (!WifiSetupIsClosed()) {
        WifiSetupCloseCurrent(false);
        return ESP_ERR_INVALID_STATE;
    }
    if (CellularBoard()) {
        return ESP_OK;
    }
    const auto result = WifiRadio::GetInstance().PauseForSleep(PowerLifecycleIsStandby());
    NotifyNetworkChanged();
    return result;
}

esp_err_t PowerLifecycleResumeNetwork() {
    if (!WifiSetupIsClosed()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (CellularBoard()) {
        PowerLifecycleResumeCellular();
        return ESP_OK;
    }
    const auto result = WifiRadio::GetInstance().ResumeFromSleep();
    NotifyNetworkChanged();
    return result;
}

bool PowerLifecycleBeginPowerOff() {
    if (s_power_off_started.exchange(true)) {
        return true;
    }
    if (FirmwareCreateTask(
            PowerOffTask, "pwr_off", kPowerOffStackBytes, nullptr, tskIDLE_PRIORITY + 5, nullptr, 0,
            kPowerOffStackCaps) != pdPASS) {
        s_power_off_started.store(false);
        ESP_LOGE(TAG, "operation=power_off stage=create_task result=no_memory");
        return false;
    }
    return true;
}

bool PowerLifecycleIsStandby() {
    return s_system_standby.load(std::memory_order_acquire);
}

void PowerLifecycleSetStandby(bool standby) {
    s_system_standby.store(standby, std::memory_order_release);
}

esp_err_t PowerLifecycleBeginBatteryShutdown(bool low_voltage) {
    if (FirmwareCreateTask(BatteryShutdownTask, "battery_off", 16 * 1024,
                                        low_voltage ? reinterpret_cast<void*>(1) : nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Battery protect: shutdown task allocation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void PowerLifecyclePrepareStandbyOverlay() {
    UiPrepareStandbyOverlay();
}

void PowerLifecyclePauseStandbyActivities() {
    UiPauseWallpaperDecode(true);
    BlePageTurnerSetStandby(true);
    UiPauseStandbyApplications();
    // Stop the log producer before closing admission. Existing file handles and
    // the explicitly permitted standby decoder finish before SD is removed.
    SdLogWriter_OnSdUnmounting();
    SdAccessQuiesce();
    UiPauseStandbyViews();
    PowerPolicy::GetInstance().RequestReevaluate();
}

void PowerLifecycleResumeStandbyStorage() {
    SdAccessResume();
    SdLogWriter_OnSdMounted();
}

void PowerLifecycleResumeStandbyActivities() {
    UiResumeStandbyCalibration();
    PowerPolicy::GetInstance().OnStandbyOverlayDismissed();
    BlePageTurnerSetStandby(false);
    UiResumeStandbyApplications();

    // 清除待机前电量，待唤醒负载稳定 3s 后重新校准。
    Bq27220Gauge::GetInstance().ResetFilter();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->UpdateStatusBar(true);
    }

    UiResumeStandbyViews();
}

void PowerLifecycleFinishStandbyResume() {
    UiFinishStandbyResume();
}

void PowerLifecycleSetStandbyResuming(bool pending) {
    if (pending) {
        const auto stats = EpdGetStats();
        s_resume_sequence.store(stats.submitted);
        s_resume_errors.store(stats.errors);
    }
    s_resuming.store(pending);
}

bool PowerLifecycleIsStandbyResuming() {
    if (!s_resuming.load()) {
        return false;
    }
    const auto stats = EpdGetStats();
    if (static_cast<int32_t>(stats.completed - s_resume_sequence.load()) > 0) {
        s_resuming.store(false);
        return false;
    }
    // 唤醒帧刷新失败时同样结束窗口，避免前台业务被永久暂停。
    if (stats.errors != s_resume_errors.load() && !stats.busy) {
        ESP_LOGW(TAG, "resume frame failed code=%d; release foreground hold", stats.last_error);
        s_resuming.store(false);
        return false;
    }
    return true;
}

bool PowerLifecycleIsPoweringOff() {
    return s_power_off_started.load();
}

const char* PowerLifecyclePendingStandbyWork() {
    if (AppNetworkBusy()) {
        return "app_network";
    }
    if (AppCameraPreviewBusy()) {
        return "camera_drain";
    }
    if (!SdAccessIsIdle()) {
        return "sd_drain";
    }
    if (SystemState::GetInstance().IsStartupBusy()) {
        return "boot_work";
    }
    if (AudioRuntime::GetInstance().HasWork() || PowerPolicy::GetInstance().HasForegroundNetworkWork() ||
        PowerPolicy::GetInstance().HasPendingHardwareWork()) {
        return "business_drain";
    }
    return nullptr;
}
