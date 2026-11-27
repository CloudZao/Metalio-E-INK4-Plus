#include <atomic>
#include <lvgl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>
#include "sysmgmt/tasks/task_executor.h"
#include "power_policy.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/power/power_hw.h"
#include "config.h"
#include "infra/storage/settings.h"
#include "power_policy_priv.h"
#include "business/diagnostics/log_upload.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "sysmgmt/system_notice.h"

static const char* TAG = "PowerPolicy";

constexpr const char* kPowerNvsNs = "power";
constexpr const char* kIdleMhzNvsKey = "idle_mhz";
constexpr const char* kNetGraceNvsKey = "net_grace_s";
constexpr const char* kStandbyOffNvsKey = "standby_off_s";
constexpr int kIdleMhzDefault = BOARD_IDLE_CPU_MHZ;

Board* power_policy_board = nullptr;
SemaphoreHandle_t power_policy_mu = nullptr;
uint8_t power_policy_need_ref[static_cast<int>(PowerNeed::kCount)] = {};
DeviceState power_policy_device_state = kDeviceStateUnknown;
PowerPolicyMode power_policy_mode = PowerPolicyMode::Full;
bool power_policy_wifi_stopped_by_policy = false;
bool power_policy_wifi_stop_pending = false;
bool power_policy_wifi_start_pending = false;
bool power_policy_main_rail_off_by_policy = false;
bool power_policy_main_rail_off_pending = false;
bool power_policy_main_rail_on_pending = false;
bool power_policy_standby_show_pending = false;  // 锁外 lv_async Show（对齐电源键）
bool power_policy_restore_provisioning_ui_pending = false;
bool power_policy_touch_asleep = false;
int power_policy_idle_cpu_mhz = kIdleMhzDefault;
int power_policy_queued_cpu_mhz = 0;
int power_policy_net_grace_sec = kPowerPolicyNetGraceSecDefault;
int power_policy_standby_to_shutdown_sec = kPowerPolicyStandbyOffSecDefault;
int64_t power_policy_last_user_activity_us = 0;
int64_t power_policy_standby_elapsed_us = 0;
int64_t power_policy_standby_awake_mark_us = 0;
bool power_policy_standby_ui_requested = false;
bool power_policy_shutdown_requested = false;
esp_timer_handle_t power_policy_tick = nullptr;
TaskHandle_t power_policy_lp_task = nullptr;
bool power_policy_lp_session_active = false;
bool power_policy_standby_wake_exit = false;

static std::atomic<int> s_cpu_mhz_pending{0};
static std::atomic<int> s_net_grace_pending{-1};
static std::atomic<int> s_standby_off_pending{-1};
static std::atomic<int> s_idle_mhz_pending{-1};
static std::atomic<bool> s_settings_writer_running{false};
static std::atomic<int64_t> s_settings_retry_at{0}; // 存储或投递失败后的限频重试时刻
static bool s_hw_flushing = false;  // 策略锁保护的硬件串行执行标记
static std::atomic<bool> s_cpu_freq_async_queued{false};
static std::atomic<bool> s_foreground_flush_queued{false};  // 合并前台保网变化，避免 UI 等待网络硬件

static bool NeedHeld(PowerNeed n) {
    return power_policy_need_ref[static_cast<int>(n)] > 0;
}

static bool AnyFullNeed() {
    return NeedHeld(PowerNeed::PhoneCall) || NeedHeld(PowerNeed::BtAudio);
}

static bool AnyNetNeed() {
    // 配网/激活态的事务需求；前台页面保网不覆盖其原有无操作待机规则。
    return NeedHeld(PowerNeed::AudioSession) || NeedHeld(PowerNeed::LocalMic) || NeedHeld(PowerNeed::OtaDownload) ||
           NeedHeld(PowerNeed::UiKeepNet) || NeedHeld(PowerNeed::PeripheralReset) || NeedHeld(PowerNeed::AudioTesting);
}

static bool DeviceWantsPerf(DeviceState st) {
    switch (st) {
        case kDeviceStateAudioTesting:
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateUpgrading:
            return true;
        default:
            return false;
    }
}

// PA：仅播报/音频测试；Connecting/Listening 保网但不拉 PA（避免进百问 PA on→off）
static bool DeviceWantsPa(DeviceState st) {
    switch (st) {
        case kDeviceStateSpeaking:
        case kDeviceStateAudioTesting:
            return true;
        default:
            return false;
    }
}

static bool NeedsHardNetActiveLocked() {
    if (NeedHeld(PowerNeed::LocalMic) || NeedHeld(PowerNeed::OtaDownload) || NeedHeld(PowerNeed::PeripheralReset) ||
        NeedHeld(PowerNeed::AudioTesting)) {
        return true;
    }
    return DeviceWantsPerf(power_policy_device_state);
}

static bool NeedsSoftKeepNetLocked() {
    return NeedHeld(PowerNeed::AudioSession) || NeedHeld(PowerNeed::UiKeepNet) ||
           NeedHeld(PowerNeed::ForegroundNetwork);
}

static bool AllowsUserIdleStandbyLocked() {
    return power_policy_device_state == kDeviceStateIdle || power_policy_device_state == kDeviceStateStarting;
}

static bool CanEnterAppIdleLocked() {
    // 本地播放依赖满频供数与功放供电，播放期间不进入空闲降频档。
    return power_policy_device_state == kDeviceStateIdle && !NeedHeld(PowerNeed::LocalPlayback);
}

static bool IsProvisioningStateLocked() {
    return power_policy_device_state == kDeviceStateWifiConfiguring ||
           power_policy_device_state == kDeviceStateActivating;
}

static bool UserInactiveLongEnoughLocked() {
    if (NeedsHardNetActiveLocked()) {
        return false;
    }
    // 虚拟 U 盘 / 显式禁待机：禁止无操作进待机（PC 挂载或产测老化可能长时间无触摸）
    if (NeedHeld(PowerNeed::UsbVirtualDisk) || NeedHeld(PowerNeed::StandbyInhibit)) {
        return false;
    }
    if (power_policy_last_user_activity_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - power_policy_last_user_activity_us) >= PowerPolicyUserIdleToStandbyUs();
}

static bool WouldPauseWifiFromAppIdleLocked() {
    if (NeedsHardNetActiveLocked() || NeedsSoftKeepNetLocked()) {
        return false;
    }
    return CanEnterAppIdleLocked();
}

static int64_t InactiveUsLocked() {
    if (power_policy_last_user_activity_us == 0) {
        return 0;
    }
    return esp_timer_get_time() - power_policy_last_user_activity_us;
}

// 普通空闲：暂留与进待机从 last_user_activity 并行计时
static bool ParallelNetGraceHoldsLocked() {
    return WouldPauseWifiFromAppIdleLocked() && InactiveUsLocked() < PowerPolicyNetGraceUs();
}

// 配网/激活：满进待机阈值后再跑暂留
static bool ProvisioningStandbyGraceHoldsLocked() {
    if (!UserInactiveLongEnoughLocked()) {
        return false;
    }
    return power_policy_net_grace_sec != 0 &&
           InactiveUsLocked() < PowerPolicyUserIdleToStandbyUs() + PowerPolicyNetGraceUs();
}

static bool ShouldStayInStandbyUiLocked() {
    if (StandbyPresentation::IsActive()) {
        return true;
    }
    if (power_policy_standby_wake_exit) {
        return false;
    }
    return power_policy_standby_ui_requested;
}

static PowerPolicyMode EvaluateProvisioningLocked() {
    if (StandbyPresentation::IsActive()) {
        return PowerPolicyMode::StandbyUi;
    }
    if (AnyNetNeed()) {
        return PowerPolicyMode::NetActive;
    }
    if (ShouldStayInStandbyUiLocked()) {
        return PowerPolicyMode::StandbyUi;
    }
    if (UserInactiveLongEnoughLocked()) {
        if (ProvisioningStandbyGraceHoldsLocked()) {
            return PowerPolicyMode::NetActive;
        }

        return PowerPolicyMode::StandbyUi;
    }
    return PowerPolicyMode::NetActive;
}

static bool IsWifiBoard() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(power_policy_board);
    return dual == nullptr || dual->GetNetworkType() == NetworkType::WIFI;
}

static void RestoreProvisioningUiAsync(void* /*arg*/) {
    UiRestoreProvisioningStatus();
}

// 与 boot_key EnterStandbyAsync 同形：无 lambda；进待机前先停传输
static void EnterStandbyUiShowCb(void* /*arg*/) {
    // The UI queue may run after a transfer has acquired its power need.
    // Never let an obsolete idle request cancel that transfer.
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool still_idle = power_policy_standby_ui_requested && power_policy_mode == PowerPolicyMode::StandbyUi &&
                            !NeedsHardNetActiveLocked() && !AnyFullNeed() && !NeedHeld(PowerNeed::StandbyInhibit) &&
                            !NeedHeld(PowerNeed::UsbVirtualDisk);
    xSemaphoreGive(power_policy_mu);
    if (!still_idle) {
        return;
    }
    LogUploadCancel();
    UiStopTransferForStandby();
    StandbyPresentation::Show();
}

static PowerPolicyMode EvaluateLocked() {
    if (AnyFullNeed()) {
        return PowerPolicyMode::Full;
    }
    if (IsProvisioningStateLocked()) {
        return EvaluateProvisioningLocked();
    }
    if (StandbyPresentation::IsActive()) {
        return PowerPolicyMode::StandbyUi;
    }
    if (NeedsHardNetActiveLocked()) {
        return PowerPolicyMode::NetActive;
    }
    if (ShouldStayInStandbyUiLocked()) {
        return PowerPolicyMode::StandbyUi;
    }
    if (AllowsUserIdleStandbyLocked() && UserInactiveLongEnoughLocked()) {
        // 并行计时：暂留已在 AppIdle 前消费，满进待机阈值直接待机，勿二次拉网

        return PowerPolicyMode::StandbyUi;
    }
    if (NeedsSoftKeepNetLocked()) {
        return PowerPolicyMode::NetActive;
    }
    if (!CanEnterAppIdleLocked()) {
        return PowerPolicyMode::NetActive;
    }
    if (power_policy_net_grace_sec == 0) {
        return PowerPolicyMode::NetActive;
    }
    // 策略已 pause WiFi 且无占网：维持 AppIdle（回首页、触摸唤醒、退出待机等）
    // 需网页靠 UiKeepNet / 各页 OnResumeFromStandby 内 EnsureNetworkReady 自行拉网
    if (power_policy_wifi_stopped_by_policy && !NeedsSoftKeepNetLocked() && !NeedsHardNetActiveLocked()) {
        return PowerPolicyMode::AppIdle;
    }
    if (ParallelNetGraceHoldsLocked()) {
        return PowerPolicyMode::NetActive;
    }

    return PowerPolicyMode::AppIdle;
}

static int NormalizeIdleMhz(int mhz) {
    if (mhz == BOARD_IDLE_CPU_LOW_MHZ || mhz == BOARD_IDLE_CPU_MID_MHZ || mhz == BOARD_IDLE_CPU_HIGH_MHZ) {
        return mhz;
    }
    return kIdleMhzDefault;
}

static int NormalizeNetGraceSec(int sec) {
    for (int option : kPowerPolicyNetGraceOptionsSec) {
        if (sec == option) {
            return sec;
        }
    }
    return kPowerPolicyNetGraceSecDefault;
}

static int NormalizeStandbyOffSec(int sec) {
    for (int option : kPowerPolicyStandbyOffOptionsSec) {
        if (sec == option) {
            return sec;
        }
    }
    return kPowerPolicyStandbyOffSecDefault;
}

static void LoadPowerPrefsFromNvs() {
    Settings settings(kPowerNvsNs, false);
    power_policy_idle_cpu_mhz = NormalizeIdleMhz(settings.GetInt(kIdleMhzNvsKey, kIdleMhzDefault));
    power_policy_net_grace_sec = NormalizeNetGraceSec(settings.GetInt(kNetGraceNvsKey, kPowerPolicyNetGraceSecDefault));
    power_policy_standby_to_shutdown_sec =
        NormalizeStandbyOffSec(settings.GetInt(kStandbyOffNvsKey, kPowerPolicyStandbyOffSecDefault));
    ESP_LOGI(TAG, "power config idle_mhz=%d idle_stb_s=%d net_grace_s=%d stb_off_s=%d", power_policy_idle_cpu_mhz,
             BOARD_IDLE_TO_STANDBY_SECONDS, power_policy_net_grace_sec, power_policy_standby_to_shutdown_sec);
}

static void PersistPowerSettings() {
    struct PowerSettingsWriterGuard {
        int net = -1; // 尚未成功提交的断网暂留时长
        int off = -1; // 尚未成功提交的自动关机时长
        int idle = -1; // 尚未成功提交的空闲CPU频率

        ~PowerSettingsWriterGuard() {
            if (net >= 0 || off >= 0 || idle >= 0) {
                int empty = -1;
                if (net >= 0) {
                    s_net_grace_pending.compare_exchange_strong(empty, net);
                }
                empty = -1;
                if (off >= 0) {
                    s_standby_off_pending.compare_exchange_strong(empty, off);
                }
                empty = -1;
                if (idle >= 0) {
                    s_idle_mhz_pending.compare_exchange_strong(empty, idle);
                }
                s_settings_retry_at.store(esp_timer_get_time() + 5LL * 1000000);
            }
            s_settings_writer_running.store(false);
        }
    } guard;
    for (;;) {
        guard.net = s_net_grace_pending.exchange(-1);
        guard.off = s_standby_off_pending.exchange(-1);
        guard.idle = s_idle_mhz_pending.exchange(-1);
        if (guard.net < 0 && guard.off < 0 && guard.idle < 0) {
            return;
        }
        Settings settings(kPowerNvsNs, true);
        const bool net_ok = guard.net < 0 || settings.SetInt(kNetGraceNvsKey, guard.net);
        const bool off_ok = guard.off < 0 || settings.SetInt(kStandbyOffNvsKey, guard.off);
        const bool idle_ok = guard.idle < 0 || settings.SetInt(kIdleMhzNvsKey, guard.idle);
        if (!net_ok || !off_ok || !idle_ok || !settings.Commit()) {
            return;
        }
        ESP_LOGI(TAG, "power settings saved net_grace_s=%d standby_off_s=%d idle_mhz=%d", guard.net, guard.off, guard.idle);
        guard.net = guard.off = guard.idle = -1;
    }
}

static void StartPowerSettingsWriter() {
    if ((s_net_grace_pending.load() < 0 && s_standby_off_pending.load() < 0 && s_idle_mhz_pending.load() < 0) ||
        esp_timer_get_time() < s_settings_retry_at.load() || s_settings_writer_running.exchange(true)) {
        return;
    }
    // 复用内部 RAM 栈的存储工作任务，避免从 LVGL 的 PSRAM 栈访问 NVS。
    if (!TaskExecutor::Schedule(PersistPowerSettings, TaskDomain::Storage)) {
        s_settings_retry_at.store(esp_timer_get_time() + 5LL * 1000000);
        s_settings_writer_running.store(false);
    }
}

static bool ApplyPendingCpuFrequency() {
    const int mhz = s_cpu_mhz_pending.exchange(0);
    if (mhz == 0) {
        return true;
    }
    const auto result = power_hw_cpu_freq_set(mhz);
    if (result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED) {
        return true;
    }
    int empty = 0;
    s_cpu_mhz_pending.compare_exchange_strong(empty, mhz);
    return false;
}

static void CpuFreqAsyncCb(void* /*arg*/) {
    s_cpu_freq_async_queued.store(false, std::memory_order_release);
    if (!ApplyPendingCpuFrequency()) {
        return;
    }
    if (s_cpu_mhz_pending.load() != 0 && !s_cpu_freq_async_queued.exchange(true, std::memory_order_acq_rel)) {
        if (!ScreenLvAsync(CpuFreqAsyncCb)) {
            s_cpu_freq_async_queued.store(false, std::memory_order_release);
            ESP_LOGW(TAG, "cpu freq re-queue failed");
        }
    }
}

static void TickCb(void* /*arg*/) {
    StartPowerSettingsWriter();
    power_hw_report_sleep_reset();
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    PowerPolicyReevaluateLocked();
    if (power_policy_mode == PowerPolicyMode::StandbyUi && PowerPolicyStandbyUiLongEnoughLocked()) {
        PowerPolicyRequestShutdownLocked();
    }
    xSemaphoreGive(power_policy_mu);
    PowerPolicyFlushCpuFreqOutsideLock();
}

static void FlushForegroundNetworkAsync() {
    if (s_foreground_flush_queued.exchange(true)) {
        return;
    }
    if (!TaskExecutor::Schedule(
            []() {
                // 硬件执行器会重读最新 pending 状态，快速离页或再次待机不会重放旧需求。
                PowerPolicyFlushCpuFreqOutsideLock();
                s_foreground_flush_queued.store(false);
            },
            TaskDomain::Network)) {
        // 需求已存入引用计数；现有策略节拍会重新评估并执行，不丢失申请或释放。
        s_foreground_flush_queued.store(false);
    }
}

static void SelfCheckThresholdsOnce() {
    if (PowerPolicyStandbyToShutdownUs() < PowerPolicyUserIdleToStandbyUs()) {
        ESP_LOGE(TAG, "self-check: shutdown threshold must not be below idle-to-standby");
    }
}

const char* PowerPolicyModeName(PowerPolicyMode m) {
    switch (m) {
        case PowerPolicyMode::Full:
            return "Full";
        case PowerPolicyMode::NetActive:
            return "NetActive";
        case PowerPolicyMode::KeepNet:
            return "KeepNet";
        case PowerPolicyMode::AppIdle:
            return "AppIdle";
        case PowerPolicyMode::StandbyUi:
            return "StandbyUi";
        default:
            return "?";
    }
}

void PowerPolicyQueueCpuMhzLocked(int mhz) {
    if (mhz == power_policy_queued_cpu_mhz) {
        return;
    }
    power_policy_queued_cpu_mhz = mhz;
    s_cpu_mhz_pending.store(mhz);
}

void PowerPolicyFlushCpuFreqOutsideLock() {
    if (s_cpu_mhz_pending.load() != 0) {
        // 优先投递 UI 队列；失败则本任务直接定频
        if (!s_cpu_freq_async_queued.exchange(true, std::memory_order_acq_rel)) {
            if (!ScreenLvAsync(CpuFreqAsyncCb)) {
                s_cpu_freq_async_queued.store(false, std::memory_order_release);
                ApplyPendingCpuFrequency();
            }
        }
    }

    // 网络操作可能阻塞；单个执行方在锁外串行执行，每步重读最新请求。
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    if (s_hw_flushing) {
        xSemaphoreGive(power_policy_mu);
        return;
    }
    s_hw_flushing = true;
    for (;;) {
        enum class Action {
            None,       // 无待执行操作
            RailOn,     // 恢复共享电源
            WifiStop,   // 暂停 WiFi
            WifiStart,  // 恢复 WiFi
            RailOff,    // 关闭共享电源
        };
        Action action = Action::None;
        if (power_policy_main_rail_on_pending) {
            power_policy_main_rail_on_pending = false;
            action = Action::RailOn;
        } else if (power_policy_wifi_stop_pending) {
            power_policy_wifi_stop_pending = false;
            action = Action::WifiStop;
        } else if (power_policy_wifi_start_pending) {
            power_policy_wifi_start_pending = false;
            action = Action::WifiStart;
        } else if (power_policy_main_rail_off_pending) {
            power_policy_main_rail_off_pending = false;
            action = Action::RailOff;
        }
        if (action == Action::None) {
            s_hw_flushing = false;
            xSemaphoreGive(power_policy_mu);
            break;
        }
        xSemaphoreGive(power_policy_mu);
        esp_err_t result = ESP_OK;
        switch (action) {
            case Action::RailOn:
                result = power_hw_main_rail_set(true);
                break;
            case Action::WifiStop:
                result = PowerLifecyclePauseNetwork();
                break;
            case Action::WifiStart:
                result = PowerLifecycleResumeNetwork();
                break;
            case Action::RailOff:
                result = power_hw_main_rail_set(false);
                break;
            case Action::None:
                break;
        }
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        if (result != ESP_OK && result != ESP_ERR_NOT_SUPPORTED) {
            // 下一策略节拍重试；新需求已反向时不重放过时动作。
            if (action == Action::RailOn && !power_policy_main_rail_off_by_policy) {
                power_policy_main_rail_on_pending = true;
            }
            if (action == Action::RailOff && power_policy_main_rail_off_by_policy) {
                power_policy_main_rail_off_pending = true;
            }
            if (action == Action::WifiStop && power_policy_wifi_stopped_by_policy) {
                power_policy_wifi_stop_pending = true;
            }
            if (action == Action::WifiStart && !power_policy_wifi_stopped_by_policy) {
                power_policy_wifi_start_pending = true;
            }
            s_hw_flushing = false;
            xSemaphoreGive(power_policy_mu);
            break;
        }
    }

    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool show_standby = power_policy_standby_show_pending;
    power_policy_standby_show_pending = false;
    const bool restore_prov = power_policy_restore_provisioning_ui_pending;
    power_policy_restore_provisioning_ui_pending = false;
    xSemaphoreGive(power_policy_mu);
    if (show_standby) {
        // 与电源键同：勿经 MainEventLoop，避免被百问等网占死
        if (!ScreenLvAsyncUrgent(EnterStandbyUiShowCb)) {
            ScreenLvAsync(EnterStandbyUiShowCb);
        }
    }
    if (restore_prov) {
        ScreenLvAsync(RestoreProvisioningUiAsync);
    }
}

bool PowerPolicy::HasPendingHardwareWork() const {
    if (power_policy_mu == nullptr) {
        return false;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool pending = s_hw_flushing || power_policy_wifi_start_pending || power_policy_wifi_stop_pending ||
                         power_policy_main_rail_on_pending || power_policy_main_rail_off_pending;
    xSemaphoreGive(power_policy_mu);
    return pending;
}

void PowerPolicyBeginPowerOffLocked() {
    LogUploadCancel();
    if (power_policy_shutdown_requested) {
        return;
    }
    // 关机第一件事：触摸深睡（再刷关机图/脉冲）；待机 LP 已睡则 no-op
    PowerPolicyEnsureTouchSleepLocked();
    power_policy_shutdown_requested = true;
    // 关机任务会自行 hold MAIN_PWR；取消未执行的掉电 pending，避免与 pwr_off 竞态
    power_policy_main_rail_off_pending = false;
    power_policy_main_rail_on_pending = false;
    if (!PowerLifecycleBeginPowerOff()) {
        power_policy_shutdown_requested = false;
        PowerPolicyWakeTouchLocked();
    }
}

/** @brief 与 want_wifi_off 同条件：AppIdle / 待机 Overlay → MAIN_PWR 掉电 */
void PowerPolicyQueueMainRailForModeLocked(bool want_off) {
    if (power_policy_shutdown_requested) {
        return;
    }
    if (want_off) {
        if (!power_policy_main_rail_off_by_policy) {
            power_policy_main_rail_off_by_policy = true;
            power_policy_main_rail_on_pending = false;
            power_policy_main_rail_off_pending = true;
        } else if (power_hw_main_rail_is_on()) {
            // EnsureNetworkReady 等路径曾旁路上电：待机 Overlay 补掉电
            power_policy_main_rail_off_pending = true;
        }
    } else if (power_policy_main_rail_off_by_policy) {
        power_policy_main_rail_off_by_policy = false;
        power_policy_main_rail_off_pending = false;
        power_policy_main_rail_on_pending = true;
    }
}

void PowerPolicyApplyLocked(PowerPolicyMode next) {
    if (power_policy_board == nullptr) {
        return;
    }

    const PowerPolicyMode prev = power_policy_mode;
    const bool want_main_rail_off =
        (next == PowerPolicyMode::AppIdle) || (next == PowerPolicyMode::StandbyUi && StandbyPresentation::IsActive());

    if (IsWifiBoard()) {
        const bool want_wifi_off = !WifiStation::GetInstance().IsUserEnabled() || next == PowerPolicyMode::AppIdle ||
                                   (next == PowerPolicyMode::StandbyUi && StandbyPresentation::IsActive());
        if (want_wifi_off) {
            if (!power_policy_wifi_stopped_by_policy) {
                power_policy_wifi_stopped_by_policy = true;
                power_policy_wifi_start_pending = false;
                power_policy_wifi_stop_pending = true;
            }
        } else if (power_policy_wifi_stopped_by_policy && next != PowerPolicyMode::StandbyUi) {
            power_policy_wifi_stopped_by_policy = false;
            power_policy_wifi_stop_pending = false;
            power_policy_wifi_start_pending = true;
            if (next == PowerPolicyMode::KeepNet) {
                ESP_LOGI(TAG, "WiFi kept for soft session (KeepNet)");
            }
        }
    }

    if (next == prev) {
        if (next == PowerPolicyMode::StandbyUi && !power_policy_standby_wake_exit && !power_policy_shutdown_requested) {
            PowerPolicyArmStandbyLpTaskLocked();
            if (StandbyPresentation::IsActive()) {
                PowerPolicyQueueMainRailForModeLocked(true);
            }
        }
        // 档位未变时仍按 DeviceState 刷 PA：Idle↔Speaking 等常停在 NetActive
        const bool want_pa = !NeedHeld(PowerNeed::PeripheralReset) && !NeedHeld(PowerNeed::UiKeepNet) &&
                             ((next == PowerPolicyMode::Full) ||
                              (next == PowerPolicyMode::NetActive &&
                               (DeviceWantsPa(power_policy_device_state) || NeedHeld(PowerNeed::AudioTesting))) ||
                              (next != PowerPolicyMode::StandbyUi && NeedHeld(PowerNeed::LocalPlayback)));
        if (!NeedHeld(PowerNeed::UiKeepNet)) {
            power_hw_pa_set(want_pa);
        }
        return;
    }

    power_policy_mode = next;
    ESP_LOGI(TAG, "mode %s -> %s", PowerPolicyModeName(prev), PowerPolicyModeName(next));

    if (prev == PowerPolicyMode::StandbyUi && next != PowerPolicyMode::StandbyUi) {
        power_policy_standby_wake_exit = false;
    }

    if (prev == PowerPolicyMode::StandbyUi && next == PowerPolicyMode::NetActive && IsProvisioningStateLocked()) {
        power_policy_restore_provisioning_ui_pending = true;
    }

    if (next == PowerPolicyMode::StandbyUi) {
        PowerPolicyResetStandbyElapsedLocked();
        PowerPolicyRequestStandbyUiAsync();
        PowerPolicyArmStandbyLpTaskLocked();
    } else {
        PowerPolicyClearStandbyElapsedLocked();
        power_policy_standby_ui_requested = false;
        power_policy_standby_show_pending = false;
        PowerPolicyWakeTouchLocked();
    }

    const int want_mhz =
        (next == PowerPolicyMode::AppIdle) ? power_policy_idle_cpu_mhz : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const bool want_pa = !NeedHeld(PowerNeed::PeripheralReset) && !NeedHeld(PowerNeed::UiKeepNet) &&
                         ((next == PowerPolicyMode::Full) ||
                          (next == PowerPolicyMode::NetActive &&
                           (DeviceWantsPa(power_policy_device_state) || NeedHeld(PowerNeed::AudioTesting))) ||
                          (next != PowerPolicyMode::StandbyUi && NeedHeld(PowerNeed::LocalPlayback)));
    PowerPolicyQueueCpuMhzLocked(want_mhz);

    if (!NeedHeld(PowerNeed::UiKeepNet)) {
        power_hw_pa_set(want_pa);
    }

    // MAIN_PWR 与保网掉电同生命周期（含 4G 板 AppIdle，不依赖 WiFi pause）
    PowerPolicyQueueMainRailForModeLocked(want_main_rail_off);
}

void PowerPolicyReevaluateLocked() {
    PowerPolicyApplyLocked(EvaluateLocked());
}

PowerPolicy& PowerPolicy::GetInstance() {
    static PowerPolicy inst;
    return inst;
}

void PowerPolicy::Init(Board* board) {
    ESP_ERROR_CHECK(power_hw_sleep_init());
    LoadPowerPrefsFromNvs();
    SelfCheckThresholdsOnce();
    power_policy_board = board;
    // 首次读取 Wi-Fi NVS 必须早于使用 PSRAM 栈的外设任务启动。
    if (IsWifiBoard()) {
        WifiStation::GetInstance();
    }
    power_policy_shutdown_requested = false;
    power_policy_lp_session_active = false;
    power_policy_last_user_activity_us = esp_timer_get_time();
    if (power_policy_mu == nullptr) {
        power_policy_mu = xSemaphoreCreateMutex();
    }
    PowerPolicyStartStandbyLpTask();
    if (power_policy_tick == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &TickCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pwr_pol",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &power_policy_tick));
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_policy_tick, 1 * 1000 * 1000));
    }
    ESP_LOGI(TAG, "init ok (user-idle %d s -> standby, net_grace %d s, LP shutdown %d s, idle_mhz=%d, light_sleep=%d)",
             BOARD_IDLE_TO_STANDBY_SECONDS, power_policy_net_grace_sec, power_policy_standby_to_shutdown_sec,
             power_policy_idle_cpu_mhz, kPowerPolicyStandbyLpUseLightSleep ? 1 : 0);
}

void PowerPolicy::Acquire(PowerNeed need) {
    if (need >= PowerNeed::kCount || power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    if (need == PowerNeed::PhoneCall || need == PowerNeed::BtAudio || need == PowerNeed::AudioSession ||
        need == PowerNeed::LocalMic || need == PowerNeed::OtaDownload || need == PowerNeed::UiKeepNet ||
        need == PowerNeed::PeripheralReset || need == PowerNeed::ForegroundNetwork) {
    }
    uint8_t& ref = power_policy_need_ref[static_cast<int>(need)];
    if (ref < 255) {
        ++ref;
    }
    if (!StandbyPresentation::IsActive() && (NeedsHardNetActiveLocked() || AnyFullNeed() ||
                                       NeedHeld(PowerNeed::StandbyInhibit) || NeedHeld(PowerNeed::UsbVirtualDisk))) {
        power_policy_standby_ui_requested = false;
        power_policy_standby_show_pending = false;
    }
    PowerPolicyReevaluateLocked();
    xSemaphoreGive(power_policy_mu);
    if (need == PowerNeed::ForegroundNetwork) {
        FlushForegroundNetworkAsync();
    } else {
        PowerPolicyFlushCpuFreqOutsideLock();
    }
}

void PowerPolicy::Release(PowerNeed need) {
    if (need >= PowerNeed::kCount || power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool hard_before = NeedsHardNetActiveLocked();
    const bool usb_before = NeedHeld(PowerNeed::UsbVirtualDisk);
    const bool inhibit_before = NeedHeld(PowerNeed::StandbyInhibit);
    uint8_t& ref = power_policy_need_ref[static_cast<int>(need)];
    if (ref > 0) {
        --ref;
    }
    // 硬占网 / 虚拟 U 盘 / 禁待机解除后刷新无操作计时，避免立刻进待机
    if ((hard_before && !NeedsHardNetActiveLocked()) || (usb_before && !NeedHeld(PowerNeed::UsbVirtualDisk)) ||
        (inhibit_before && !NeedHeld(PowerNeed::StandbyInhibit))) {
        power_policy_last_user_activity_us = esp_timer_get_time();
    }
    // 等网任务可能在待机请求后才完成 Resume；最后归还前校正实际 STA 状态。
    if (need == PowerNeed::ForegroundNetwork && StandbyPresentation::IsActive() && IsWifiBoard() &&
        !WifiStation::GetInstance().IsLpPaused()) {
        power_policy_wifi_stopped_by_policy = false;
    }
    PowerPolicyReevaluateLocked();
    xSemaphoreGive(power_policy_mu);
    if (need == PowerNeed::ForegroundNetwork) {
        FlushForegroundNetworkAsync();
    } else {
        PowerPolicyFlushCpuFreqOutsideLock();
    }
}

bool PowerPolicy::HasForegroundNetworkWork() const {
    if (power_policy_mu == nullptr) {
        return false;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool held = NeedHeld(PowerNeed::ForegroundNetwork);
    xSemaphoreGive(power_policy_mu);
    return held;
}

void PowerPolicy::NotifyDeviceState(DeviceState state) {
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool perf_before = DeviceWantsPerf(power_policy_device_state);
    power_policy_device_state = state;
    // 聆听/播报期间 DeviceWantsPerf 挡进待机，但不刷新无操作计时；落 Idle 时补刷，避免松手立刻进浅睡
    if (perf_before && !DeviceWantsPerf(state)) {
        power_policy_last_user_activity_us = esp_timer_get_time();
    }
    PowerPolicyReevaluateLocked();
    xSemaphoreGive(power_policy_mu);
    PowerPolicyFlushCpuFreqOutsideLock();
}

void PowerPolicy::NotifyUserActivity() {
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();

    if (power_policy_touch_asleep && power_policy_board != nullptr) {
        if (power_policy_board->TouchWakeByReset() == ESP_OK) {
            power_policy_touch_asleep = false;
        }
    }

    if (power_policy_mode == PowerPolicyMode::StandbyUi) {
        // 对齐 397 浅睡语义：退出待机只靠 POWER（EXT1/P17）；触摸深睡未接时勿点屏误 dismiss
        xSemaphoreGive(power_policy_mu);
        return;
    }

    power_policy_last_user_activity_us = now;

    xSemaphoreGive(power_policy_mu);
}

bool PowerPolicy::IsTouchAsleep() const {
    if (power_policy_mu == nullptr) {
        return false;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const bool asleep = power_policy_touch_asleep;
    xSemaphoreGive(power_policy_mu);
    return asleep;
}

void PowerPolicy::RequestReevaluate() {
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    PowerPolicyReevaluateLocked();
    xSemaphoreGive(power_policy_mu);
    PowerPolicyFlushCpuFreqOutsideLock();
}

void PowerPolicy::OnStandbyOverlayDismissed() {
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    power_policy_standby_ui_requested = false;
    PowerPolicyClearStandbyElapsedLocked();
    power_policy_standby_wake_exit = true;
    power_policy_last_user_activity_us = esp_timer_get_time();

    PowerPolicyReevaluateLocked();
    power_policy_standby_wake_exit = false;
    xSemaphoreGive(power_policy_mu);
    PowerPolicyFlushCpuFreqOutsideLock();
}

void PowerPolicy::RequestPowerOff() {
    LogUploadCancel();
    if (power_policy_mu == nullptr) {
        PowerLifecycleBeginPowerOff();
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    ESP_LOGW(TAG, "RequestPowerOff");
    PowerPolicyBeginPowerOffLocked();
    xSemaphoreGive(power_policy_mu);
}

void PowerPolicy::OnPowerOffFailed() {
    if (!power_policy_mu) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    power_policy_shutdown_requested = false;
    PowerPolicyWakeTouchLocked();
    power_policy_last_user_activity_us = esp_timer_get_time();
    PowerPolicyResetStandbyElapsedLocked();
    xSemaphoreGive(power_policy_mu);
    if (StandbyPresentation::IsActive()) {
        PowerPolicyRequestDismissStandbyOverlay();
    }
    RequestReevaluate();
}

int PowerPolicy::GetIdleCpuMhz() const {
    return power_policy_idle_cpu_mhz;
}

void PowerPolicy::SetIdleCpuMhz(int mhz) {
    mhz = NormalizeIdleMhz(mhz);
    if (power_policy_mu == nullptr) {
        if (power_policy_idle_cpu_mhz == mhz) {
            return;
        }
        power_policy_idle_cpu_mhz = mhz;
    } else {
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
        if (power_policy_idle_cpu_mhz == mhz) {
            xSemaphoreGive(power_policy_mu);
            return;
        }
        power_policy_idle_cpu_mhz = mhz;
        if (power_policy_mode == PowerPolicyMode::AppIdle) {
            PowerPolicyQueueCpuMhzLocked(mhz);
        }
        xSemaphoreGive(power_policy_mu);
        PowerPolicyFlushCpuFreqOutsideLock();
    }
    s_idle_mhz_pending.store(mhz);
    StartPowerSettingsWriter();
}

int PowerPolicy::GetNetGraceSec() const {
    if (power_policy_mu == nullptr) {
        return power_policy_net_grace_sec;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const int sec = power_policy_net_grace_sec;
    xSemaphoreGive(power_policy_mu);
    return sec;
}

void PowerPolicy::SetNetGraceSec(int sec) {
    sec = NormalizeNetGraceSec(sec);
    if (power_policy_mu != nullptr) {
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    }
    if (power_policy_net_grace_sec != sec) {
        power_policy_net_grace_sec = sec;
        s_net_grace_pending.store(sec);
        if (power_policy_mu != nullptr) {
            // 原子更新策略并取消旧请求；在途停网由串行执行方补偿恢复。
            PowerPolicyReevaluateLocked();
        }
    }
    if (power_policy_mu != nullptr) {
        xSemaphoreGive(power_policy_mu);
        PowerPolicyFlushCpuFreqOutsideLock();
    }
    StartPowerSettingsWriter();
}

int PowerPolicy::GetStandbyToShutdownSec() const {
    if (power_policy_mu == nullptr) {
        return power_policy_standby_to_shutdown_sec;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    const int sec = power_policy_standby_to_shutdown_sec;
    xSemaphoreGive(power_policy_mu);
    return sec;
}

void PowerPolicy::SetStandbyToShutdownSec(int sec) {
    sec = NormalizeStandbyOffSec(sec);
    if (power_policy_mu != nullptr) {
        xSemaphoreTake(power_policy_mu, portMAX_DELAY);
    }
    if (power_policy_standby_to_shutdown_sec != sec) {
        power_policy_standby_to_shutdown_sec = sec;
        s_standby_off_pending.store(sec);
    }
    if (power_policy_mu != nullptr) {
        xSemaphoreGive(power_policy_mu);
    }
    StartPowerSettingsWriter();
}

void PowerPolicy::PreparePowerKeyStandby() {
    if (power_policy_mu == nullptr) {
        return;
    }
    xSemaphoreTake(power_policy_mu, portMAX_DELAY);

    xSemaphoreGive(power_policy_mu);
}
