#include "power_policy.h"

#include "power_policy_priv.h"
#include "power_hw.h"
#include "application.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "standby_screen/standby_screen.h"
#include "cloud_screen/cloud_screen.h"
#include "screen_common.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

static const char* TAG = "PowerPolicy";

constexpr const char* kPowerNvsNs = "power";
constexpr const char* kIdleMhzNvsKey = "idle_mhz";
constexpr const char* kIdleToStandbyNvsKey = "idle_stb_s";
constexpr const char* kNetGraceNvsKey = "net_grace_s";
constexpr const char* kStandbyToOffNvsKey = "stb_off_s";
constexpr int kIdleMhzDefault = 320;

namespace pwr {

Board* board = nullptr;
SemaphoreHandle_t mu = nullptr;
uint8_t need_ref[static_cast<int>(PowerNeed::kCount)] = {};
DeviceState device_state = kDeviceStateUnknown;
Mode mode = Mode::Full;
bool wifi_stopped_by_policy = false;
bool wifi_stop_pending = false;
bool wifi_start_pending = false;
bool main_rail_off_by_policy = false;
bool main_rail_off_pending = false;
bool main_rail_on_pending = false;
bool standby_show_pending = false;           // 锁外 lv_async Show（对齐电源键）
bool restore_provisioning_ui_pending = false;
bool touch_asleep = false;
int idle_cpu_mhz = kIdleMhzDefault;
int queued_cpu_mhz = 0;
int idle_to_standby_sec = kIdleToStandbySecDefault;
int net_grace_sec = kNetGraceSecDefault;
int standby_to_shutdown_sec = kStandbyToShutdownSecDefault;
int64_t last_user_activity_us = 0;
int64_t standby_elapsed_us = 0;
int64_t standby_awake_mark_us = 0;
bool standby_ui_requested = false;
bool shutdown_requested = false;
esp_timer_handle_t tick = nullptr;
TaskHandle_t lp_task = nullptr;
bool lp_session_active = false;
bool standby_wake_exit = false;

}  // namespace pwr

namespace {

std::atomic<int> s_cpu_mhz_pending{0};
std::atomic<bool> s_cpu_freq_async_queued{false};

bool NeedHeld(PowerNeed n)
{
    return pwr::need_ref[static_cast<int>(n)] > 0;
}

bool AnyFullNeed()
{
    return NeedHeld(PowerNeed::PhoneCall) || NeedHeld(PowerNeed::BtAudio);
}

bool AnyNetNeed()
{
    return NeedHeld(PowerNeed::AudioSession) || NeedHeld(PowerNeed::LocalMic) ||
           NeedHeld(PowerNeed::OtaDownload) || NeedHeld(PowerNeed::UiKeepNet);
}

bool DeviceWantsPerf(DeviceState st)
{
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

/** PA：仅播报/音频测试；Connecting/Listening 保网但不拉 PA（避免进百问 PA on→off） */
bool DeviceWantsPa(DeviceState st)
{
    switch (st) {
        case kDeviceStateSpeaking:
        case kDeviceStateAudioTesting:
            return true;
        default:
            return false;
    }
}

bool NeedsHardNetActiveLocked()
{
    if (NeedHeld(PowerNeed::LocalMic) || NeedHeld(PowerNeed::OtaDownload)) {
        return true;
    }
    return DeviceWantsPerf(pwr::device_state);
}

bool NeedsSoftKeepNetLocked()
{
    return NeedHeld(PowerNeed::AudioSession) || NeedHeld(PowerNeed::UiKeepNet);
}

bool AllowsUserIdleStandbyLocked()
{
    return pwr::device_state == kDeviceStateIdle || pwr::device_state == kDeviceStateStarting;
}

bool CanEnterAppIdleLocked()
{
    return pwr::device_state == kDeviceStateIdle;
}

bool IsProvisioningStateLocked()
{
    return pwr::device_state == kDeviceStateWifiConfiguring ||
           pwr::device_state == kDeviceStateActivating;
}

bool UserInactiveLongEnoughLocked()
{
    if (NeedsHardNetActiveLocked()) {
        return false;
    }
    // 虚拟 U 盘 / 显式禁待机：禁止无操作进待机（PC 挂载或产测老化可能长时间无触摸）
    if (NeedHeld(PowerNeed::UsbVirtualDisk) || NeedHeld(PowerNeed::StandbyInhibit)) {
        return false;
    }
    if (pwr::last_user_activity_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - pwr::last_user_activity_us) >= pwr::UserIdleToStandbyUs();
}

bool WouldPauseWifiFromAppIdleLocked()
{
    if (NeedsHardNetActiveLocked() || NeedsSoftKeepNetLocked()) {
        return false;
    }
    return CanEnterAppIdleLocked();
}

int64_t InactiveUsLocked()
{
    if (pwr::last_user_activity_us == 0) {
        return 0;
    }
    return esp_timer_get_time() - pwr::last_user_activity_us;
}

// 普通空闲：暂留与进待机从 last_user_activity 并行计时
bool ParallelNetGraceHoldsLocked()
{
    return WouldPauseWifiFromAppIdleLocked() && InactiveUsLocked() < pwr::NetGraceUs();
}

// 配网/激活：满进待机阈值后再跑暂留
bool ProvisioningStandbyGraceHoldsLocked()
{
    if (!UserInactiveLongEnoughLocked()) {
        return false;
    }
    return InactiveUsLocked() < pwr::UserIdleToStandbyUs() + pwr::NetGraceUs();
}

bool ShouldStayInStandbyUiLocked()
{
    if (StandbyScreen::IsActive()) {
        return true;
    }
    if (pwr::standby_wake_exit) {
        return false;
    }
    return pwr::standby_ui_requested;
}

pwr::Mode EvaluateProvisioningLocked()
{
    if (StandbyScreen::IsActive()) {
        return pwr::Mode::StandbyUi;
    }
    if (AnyNetNeed()) {
        pwr::ClearNetGraceLocked();
        return pwr::Mode::NetActive;
    }
    if (ShouldStayInStandbyUiLocked()) {
        return pwr::Mode::StandbyUi;
    }
    if (UserInactiveLongEnoughLocked()) {
        if (ProvisioningStandbyGraceHoldsLocked()) {
            return pwr::Mode::NetActive;
        }
        pwr::ClearNetGraceLocked();
        return pwr::Mode::StandbyUi;
    }
    return pwr::Mode::NetActive;
}

bool IsWifiBoard()
{
    return true; // S31 仅 WifiBoard
}

void RestoreWifiConfigUiAsync(void* /*arg*/)
{
    if (Application::GetInstance().GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;
    }
    Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, Lang::Strings::NEED_WIFI_CFG,
                                     "gear");
}

void RestoreProvisioningUiAsync(void* /*arg*/)
{
    const DeviceState st = Application::GetInstance().GetDeviceState();
    if (st == kDeviceStateWifiConfiguring) {
        RestoreWifiConfigUiAsync(nullptr);
    } else if (st == kDeviceStateActivating) {
        Application::GetInstance().ResumeActivationAfterStandby();
    }
}

/** 与 boot_key EnterStandbyAsync 同形：无 lambda；进待机前先停传输 */
void EnterStandbyUiShowCb(void* /*arg*/)
{
    CloudScreen::StopTransfer();
    StandbyScreen::Show();
}

pwr::Mode EvaluateLocked()
{
    if (AnyFullNeed()) {
        return pwr::Mode::Full;
    }
    if (IsProvisioningStateLocked()) {
        return EvaluateProvisioningLocked();
    }
    if (StandbyScreen::IsActive()) {
        return pwr::Mode::StandbyUi;
    }
    if (NeedsHardNetActiveLocked()) {
        pwr::ClearNetGraceLocked();
        return pwr::Mode::NetActive;
    }
    if (ShouldStayInStandbyUiLocked()) {
        return pwr::Mode::StandbyUi;
    }
    if (AllowsUserIdleStandbyLocked() && UserInactiveLongEnoughLocked()) {
        // 并行计时：暂留已在 AppIdle 前消费，满进待机阈值直接待机，勿二次拉网
        pwr::ClearNetGraceLocked();
        return pwr::Mode::StandbyUi;
    }
    if (NeedsSoftKeepNetLocked()) {
        pwr::ClearNetGraceLocked();
        return pwr::Mode::NetActive;
    }
    if (!CanEnterAppIdleLocked()) {
        return pwr::Mode::NetActive;
    }
    // 策略已 pause WiFi 且无占网：维持 AppIdle（回首页、触摸唤醒、退出待机等）
    // 需网页靠 UiKeepNet / 各页 OnResumeFromStandby 内 EnsureNetworkReady 自行拉网
    if (pwr::wifi_stopped_by_policy && !NeedsSoftKeepNetLocked() && !NeedsHardNetActiveLocked()) {
        return pwr::Mode::AppIdle;
    }
    if (ParallelNetGraceHoldsLocked()) {
        return pwr::Mode::NetActive;
    }
    pwr::ClearNetGraceLocked();
    return pwr::Mode::AppIdle;
}

int NormalizeIdleMhz(int mhz)
{
    if (mhz == 160 || mhz == 240 || mhz == 320) {
        return mhz;
    }
    return kIdleMhzDefault;
}

int NormalizeIdleToStandbySec(int sec)
{
    if (sec == 3 * 60 || sec == 10 * 60 || sec == 30 * 60) {
        return sec;
    }
    return pwr::kIdleToStandbySecDefault;
}

int NormalizeStandbyToShutdownSec(int sec)
{
    // 0=永不；非法 / 旧 NVS（含 30s、1h 等）归一为默认
    if (sec == 0 || sec == 3 * 60 || sec == 10 * 60 || sec == 30 * 60 ||
        sec == 3 * 60 * 60 || sec == 6 * 60 * 60) {
        return sec;
    }
    return pwr::kStandbyToShutdownSecDefault;
}

int NormalizeNetGraceSec(int sec)
{
    if (sec == 30 || sec == 60 || sec == 120) {
        return sec;
    }
    return pwr::kNetGraceSecDefault;
}

void LoadPowerPrefsFromNvs()
{
    Settings settings(kPowerNvsNs, false);
    pwr::idle_cpu_mhz = NormalizeIdleMhz(settings.GetInt(kIdleMhzNvsKey, kIdleMhzDefault));
    pwr::idle_to_standby_sec =
        NormalizeIdleToStandbySec(settings.GetInt(kIdleToStandbyNvsKey, pwr::kIdleToStandbySecDefault));
    pwr::net_grace_sec =
        NormalizeNetGraceSec(settings.GetInt(kNetGraceNvsKey, pwr::kNetGraceSecDefault));
    pwr::standby_to_shutdown_sec = NormalizeStandbyToShutdownSec(
        settings.GetInt(kStandbyToOffNvsKey, pwr::kStandbyToShutdownSecDefault));
    ESP_LOGI(TAG, "nvs load idle_mhz=%d idle_stb_s=%d net_grace_s=%d stb_off_s=%d", pwr::idle_cpu_mhz,
             pwr::idle_to_standby_sec, pwr::net_grace_sec, pwr::standby_to_shutdown_sec);
}

// LVGL worker 栈在 PSRAM：禁止在该任务里直接写 NVS（会关 cache 触发断言）。
void PersistIdleMhzTask(void* arg)
{
    const int mhz = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kPowerNvsNs, true);
    settings.SetInt(kIdleMhzNvsKey, mhz);
    ESP_LOGI(TAG, "nvs save idle_mhz=%d", mhz);
    vTaskDelete(nullptr);
}

void PersistIdleToStandbyTask(void* arg)
{
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kPowerNvsNs, true);
    settings.SetInt(kIdleToStandbyNvsKey, sec);
    ESP_LOGI(TAG, "nvs save idle_stb_s=%d", sec);
    vTaskDelete(nullptr);
}

void PersistNetGraceTask(void* arg)
{
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kPowerNvsNs, true);
    settings.SetInt(kNetGraceNvsKey, sec);
    ESP_LOGI(TAG, "nvs save net_grace_s=%d", sec);
    vTaskDelete(nullptr);
}

void PersistStandbyToOffTask(void* arg)
{
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kPowerNvsNs, true);
    settings.SetInt(kStandbyToOffNvsKey, sec);
    ESP_LOGI(TAG, "nvs save stb_off_s=%d", sec);
    vTaskDelete(nullptr);
}

void CpuFreqAsyncCb(void* /*arg*/)
{
    s_cpu_freq_async_queued.store(false, std::memory_order_release);
    const int mhz = s_cpu_mhz_pending.exchange(0);
    if (mhz != 0) {
        power_hw_cpu_freq_set(mhz);
    }
    if (s_cpu_mhz_pending.load() != 0 &&
        !s_cpu_freq_async_queued.exchange(true, std::memory_order_acq_rel)) {
        if (!ScreenLvAsync(CpuFreqAsyncCb)) {
            s_cpu_freq_async_queued.store(false, std::memory_order_release);
            ESP_LOGW(TAG, "cpu freq re-queue failed");
        }
    }
}

void TickCb(void* /*arg*/)
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    pwr::ReevaluateLocked();
    if (pwr::mode == pwr::Mode::StandbyUi && pwr::StandbyUiLongEnoughLocked()) {
        pwr::RequestShutdownLocked();
    }
    xSemaphoreGive(pwr::mu);
    pwr::FlushCpuFreqOutsideLock();
}

void SelfCheckThresholdsOnce()
{
    if (pwr::standby_to_shutdown_sec > 0 &&
        pwr::StandbyToShutdownUs() < pwr::UserIdleToStandbyUs()) {
        ESP_LOGE(TAG, "self-check: shutdown threshold must not be below idle-to-standby");
    }
}

}  // namespace

namespace pwr {

void ClearNetGraceLocked()
{
}

const char* ModeName(Mode m)
{
    switch (m) {
        case Mode::Full:
            return "Full";
        case Mode::NetActive:
            return "NetActive";
        case Mode::KeepNet:
            return "KeepNet";
        case Mode::AppIdle:
            return "AppIdle";
        case Mode::StandbyUi:
            return "StandbyUi";
        default:
            return "?";
    }
}

void QueueCpuMhzLocked(int mhz)
{
    if (mhz == queued_cpu_mhz) {
        return;
    }
    queued_cpu_mhz = mhz;
    s_cpu_mhz_pending.store(mhz);
}

void FlushCpuFreqOutsideLock()
{
    if (s_cpu_mhz_pending.load() != 0) {
        // 优先投递 UI 队列；失败则本任务直接定频
        if (!s_cpu_freq_async_queued.exchange(true, std::memory_order_acq_rel)) {
            if (!ScreenLvAsync(CpuFreqAsyncCb)) {
                s_cpu_freq_async_queued.store(false, std::memory_order_release);
                const int mhz = s_cpu_mhz_pending.exchange(0);
                if (mhz != 0) {
                    power_hw_cpu_freq_set(mhz);
                }
            }
        }
    }

    // PauseForLp 含 httpd/DNS 等待，必须在策略锁外执行
    // 轨序：上电 → 启 WiFi；停 WiFi → 掉电（避免射频/外设在无电时被访问）
    if (main_rail_on_pending) {
        main_rail_on_pending = false;
        power_hw_main_rail_set(true);
    }
    if (wifi_stop_pending) {
        wifi_stop_pending = false;
        power_hw_wifi_stop();
        ESP_LOGI(TAG, "WiFi paused (idle / standby LP)");
    }
    if (wifi_start_pending) {
        wifi_start_pending = false;
        power_hw_wifi_start();
    }
    if (main_rail_off_pending) {
        main_rail_off_pending = false;
        power_hw_main_rail_set(false);
    }

    bool show_standby = false;
    bool restore_prov = false;
    if (mu != nullptr) {
        xSemaphoreTake(mu, portMAX_DELAY);
        show_standby = standby_show_pending;
        standby_show_pending = false;
        restore_prov = restore_provisioning_ui_pending;
        restore_provisioning_ui_pending = false;
        xSemaphoreGive(mu);
    }
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

void BeginPowerOffLocked()
{
    if (shutdown_requested) {
        return;
    }
    // 关机第一件事：触摸深睡（再刷关机图/脉冲）；待机 LP 已睡则 no-op
    EnsureTouchSleepLocked();
    shutdown_requested = true;
    // 关机任务会自行 hold MAIN_PWR；取消未执行的掉电 pending，避免与 pwr_off 竞态
    main_rail_off_pending = false;
    main_rail_on_pending = false;
    power_hw_begin_power_off();
}

/** 与 want_wifi_off 同条件：AppIdle / 待机 Overlay → MAIN_PWR 掉电 */
void QueueMainRailForModeLocked(bool want_off)
{
    if (shutdown_requested) {
        return;
    }
    if (want_off) {
        if (!main_rail_off_by_policy) {
            main_rail_off_by_policy = true;
            main_rail_on_pending = false;
            main_rail_off_pending = true;
        } else if (power_hw_main_rail_is_on()) {
            // EnsureNetworkReady 等路径曾旁路上电：待机 Overlay 补掉电
            main_rail_off_pending = true;
        }
    } else if (main_rail_off_by_policy) {
        main_rail_off_by_policy = false;
        main_rail_off_pending = false;
        main_rail_on_pending = true;
    }
}

void ApplyLocked(Mode next)
{
    if (board == nullptr) {
        return;
    }

    const Mode prev = mode;
    const bool want_main_rail_off =
        (next == Mode::AppIdle) ||
        (next == Mode::StandbyUi && StandbyScreen::IsActive());

    if (next == prev) {
        if (next == Mode::StandbyUi && !standby_wake_exit && !shutdown_requested) {
            ArmStandbyLpTaskLocked();
            // 空闲先进 StandbyUi 再 Show：等 Overlay 就绪后补停网（对齐电源键路径）
            if (StandbyScreen::IsActive() && IsWifiBoard() && !wifi_stopped_by_policy) {
                wifi_stopped_by_policy = true;
                wifi_start_pending = false;
                wifi_stop_pending = true;
            }
            if (StandbyScreen::IsActive()) {
                QueueMainRailForModeLocked(true);
            }
        }
        // 档位未变时仍按 DeviceState 刷 PA：Idle↔Speaking 等常停在 NetActive
        const bool want_pa =
            !NeedHeld(PowerNeed::UiKeepNet) &&
            ((next == Mode::Full) || (next == Mode::NetActive && DeviceWantsPa(device_state)));
        if (!NeedHeld(PowerNeed::UiKeepNet)) {
            power_hw_pa_set(want_pa);
        }
        return;
    }

    mode = next;
    ESP_LOGI(TAG, "mode %s -> %s", ModeName(prev), ModeName(next));

    if (prev == Mode::StandbyUi && next != Mode::StandbyUi) {
        standby_wake_exit = false;
    }

    if (prev == Mode::StandbyUi && next == Mode::NetActive && IsProvisioningStateLocked()) {
        restore_provisioning_ui_pending = true;
    }

    if (next == Mode::StandbyUi) {
        ResetStandbyElapsedLocked();
        RequestStandbyUiAsync();
        ArmStandbyLpTaskLocked();
    } else {
        ClearStandbyElapsedLocked();
        standby_ui_requested = false;
        standby_show_pending = false;
        WakeTouchLocked();
    }

    const int want_mhz = (next == Mode::AppIdle) ? idle_cpu_mhz : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const bool want_pa =
        !NeedHeld(PowerNeed::UiKeepNet) &&
        ((next == Mode::Full) || (next == Mode::NetActive && DeviceWantsPa(device_state)));
    // 待机须 Overlay 已起来再停网（电源键短按就是先 Show 再 Reevaluate）
    const bool want_wifi_off =
        (next == Mode::AppIdle) ||
        (next == Mode::StandbyUi && StandbyScreen::IsActive());

    QueueCpuMhzLocked(want_mhz);

    if (!NeedHeld(PowerNeed::UiKeepNet)) {
        power_hw_pa_set(want_pa);
    }

    // MAIN_PWR 与保网掉电同生命周期（含 4G 板 AppIdle，不依赖 WiFi pause）
    QueueMainRailForModeLocked(want_main_rail_off);

    if (!IsWifiBoard()) {
        return;
    }

    if (want_wifi_off) {
        if (!wifi_stopped_by_policy) {
            wifi_stopped_by_policy = true;
            wifi_start_pending = false;
            wifi_stop_pending = true;
        }
        return;
    }

    if (wifi_stopped_by_policy && next != Mode::StandbyUi) {
        wifi_stopped_by_policy = false;
        wifi_stop_pending = false;
        wifi_start_pending = true;
        if (next == Mode::KeepNet) {
            ESP_LOGI(TAG, "WiFi kept for soft session (KeepNet)");
        }
    }
}

void ReevaluateLocked()
{
    ApplyLocked(EvaluateLocked());
}

}  // namespace pwr

PowerPolicy& PowerPolicy::GetInstance()
{
    static PowerPolicy inst;
    return inst;
}

void PowerPolicy::Init(Board* board)
{
    LoadPowerPrefsFromNvs();
    SelfCheckThresholdsOnce();
    pwr::board = board;
    pwr::shutdown_requested = false;
    pwr::lp_session_active = false;
    pwr::last_user_activity_us = esp_timer_get_time();
    if (pwr::mu == nullptr) {
        pwr::mu = xSemaphoreCreateMutex();
    }
    pwr::StartStandbyLpTask();
    if (pwr::tick == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &TickCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pwr_pol",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &pwr::tick));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pwr::tick, 1 * 1000 * 1000));
    }
    ESP_LOGI(TAG,
             "init ok (user-idle %d s -> standby, net_grace %d s, LP shutdown %d s, idle_mhz=%d, light_sleep=%d)",
             pwr::idle_to_standby_sec, pwr::net_grace_sec, pwr::standby_to_shutdown_sec, pwr::idle_cpu_mhz,
             pwr::kStandbyLpUseLightSleep ? 1 : 0);
}

void PowerPolicy::Acquire(PowerNeed need)
{
    if (need >= PowerNeed::kCount || pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    if (need == PowerNeed::PhoneCall || need == PowerNeed::BtAudio || need == PowerNeed::AudioSession ||
        need == PowerNeed::LocalMic || need == PowerNeed::OtaDownload || need == PowerNeed::UiKeepNet) {
        pwr::ClearNetGraceLocked();
    }
    uint8_t& ref = pwr::need_ref[static_cast<int>(need)];
    if (ref < 255) {
        ++ref;
    }
    pwr::ReevaluateLocked();
    xSemaphoreGive(pwr::mu);
    pwr::FlushCpuFreqOutsideLock();
}

void PowerPolicy::Release(PowerNeed need)
{
    if (need >= PowerNeed::kCount || pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const bool hard_before = NeedsHardNetActiveLocked();
    const bool usb_before = NeedHeld(PowerNeed::UsbVirtualDisk);
    const bool inhibit_before = NeedHeld(PowerNeed::StandbyInhibit);
    uint8_t& ref = pwr::need_ref[static_cast<int>(need)];
    if (ref > 0) {
        --ref;
    }
    // 硬占网 / 虚拟 U 盘 / 禁待机解除后刷新无操作计时，避免立刻进待机
    if ((hard_before && !NeedsHardNetActiveLocked()) ||
        (usb_before && !NeedHeld(PowerNeed::UsbVirtualDisk)) ||
        (inhibit_before && !NeedHeld(PowerNeed::StandbyInhibit))) {
        pwr::last_user_activity_us = esp_timer_get_time();
    }
    pwr::ReevaluateLocked();
    xSemaphoreGive(pwr::mu);
    pwr::FlushCpuFreqOutsideLock();
}

void PowerPolicy::NotifyDeviceState(DeviceState state)
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const bool perf_before = DeviceWantsPerf(pwr::device_state);
    pwr::device_state = state;
    // 聆听/播报期间 DeviceWantsPerf 挡进待机，但不刷新无操作计时；落 Idle 时补刷，避免松手立刻进浅睡
    if (perf_before && !DeviceWantsPerf(state)) {
        pwr::last_user_activity_us = esp_timer_get_time();
    }
    pwr::ReevaluateLocked();
    xSemaphoreGive(pwr::mu);
    pwr::FlushCpuFreqOutsideLock();
}

void PowerPolicy::NotifyUserActivity()
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();

    if (pwr::touch_asleep && pwr::board != nullptr) {
        if (pwr::board->TouchWakeByReset() == ESP_OK) {
            pwr::touch_asleep = false;
        }
    }

    if (pwr::mode == pwr::Mode::StandbyUi) {
        // 对齐 397 浅睡语义：退出待机只靠 POWER（EXT1/P17）；触摸深睡未接时勿点屏误 dismiss
        xSemaphoreGive(pwr::mu);
        return;
    }

    pwr::last_user_activity_us = now;
    pwr::ClearNetGraceLocked();
    xSemaphoreGive(pwr::mu);
}

bool PowerPolicy::IsTouchAsleep() const
{
    if (pwr::mu == nullptr) {
        return false;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    const bool asleep = pwr::touch_asleep;
    xSemaphoreGive(pwr::mu);
    return asleep;
}

void PowerPolicy::PreparePowerKeyStandby()
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    pwr::ClearNetGraceLocked();
    xSemaphoreGive(pwr::mu);
}

void PowerPolicy::RequestReevaluate()
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    pwr::ReevaluateLocked();
    xSemaphoreGive(pwr::mu);
    pwr::FlushCpuFreqOutsideLock();
}

void PowerPolicy::RequestPowerOff()
{
    if (pwr::mu == nullptr) {
        power_hw_begin_power_off();
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    ESP_LOGW(TAG, "RequestPowerOff");
    pwr::BeginPowerOffLocked();
    xSemaphoreGive(pwr::mu);
}

void PowerPolicy::OnStandbyOverlayDismissed()
{
    if (pwr::mu == nullptr) {
        return;
    }
    xSemaphoreTake(pwr::mu, portMAX_DELAY);
    pwr::standby_ui_requested = false;
    pwr::ClearStandbyElapsedLocked();
    pwr::standby_wake_exit = true;
    pwr::last_user_activity_us = esp_timer_get_time();
    pwr::ClearNetGraceLocked();
    pwr::ReevaluateLocked();
    pwr::standby_wake_exit = false;
    xSemaphoreGive(pwr::mu);
    pwr::ResumeIotKeysIfNeeded();
    pwr::FlushCpuFreqOutsideLock();
}

int PowerPolicy::GetIdleCpuMhz() const
{
    return pwr::idle_cpu_mhz;
}

int PowerPolicy::GetUserIdleToStandbySec() const
{
    return pwr::idle_to_standby_sec;
}

int PowerPolicy::GetStandbyToShutdownSec() const
{
    return pwr::standby_to_shutdown_sec;
}

int PowerPolicy::GetNetGraceSec() const
{
    return pwr::net_grace_sec;
}

void PowerPolicy::SetIdleCpuMhz(int mhz)
{
    mhz = NormalizeIdleMhz(mhz);
    if (pwr::mu == nullptr) {
        if (pwr::idle_cpu_mhz == mhz) {
            return;
        }
        pwr::idle_cpu_mhz = mhz;
    } else {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        if (pwr::idle_cpu_mhz == mhz) {
            xSemaphoreGive(pwr::mu);
            return;
        }
        pwr::idle_cpu_mhz = mhz;
        if (pwr::mode == pwr::Mode::AppIdle) {
            pwr::QueueCpuMhzLocked(mhz);
        }
        xSemaphoreGive(pwr::mu);
        pwr::FlushCpuFreqOutsideLock();
    }
    if (xTaskCreate(PersistIdleMhzTask, "idle_mhz_nvs", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(mhz)), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(idle_mhz_nvs) failed");
    }
}

void PowerPolicy::SetUserIdleToStandbySec(int sec)
{
    sec = NormalizeIdleToStandbySec(sec);
    if (pwr::mu == nullptr) {
        if (pwr::idle_to_standby_sec == sec) {
            return;
        }
        pwr::idle_to_standby_sec = sec;
    } else {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        if (pwr::idle_to_standby_sec == sec) {
            xSemaphoreGive(pwr::mu);
            return;
        }
        pwr::idle_to_standby_sec = sec;
        xSemaphoreGive(pwr::mu);
    }
    if (xTaskCreate(PersistIdleToStandbyTask, "idle_stb_nvs", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(sec)), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(idle_stb_nvs) failed");
    }
}

void PowerPolicy::SetNetGraceSec(int sec)
{
    sec = NormalizeNetGraceSec(sec);
    if (pwr::mu == nullptr) {
        if (pwr::net_grace_sec == sec) {
            return;
        }
        pwr::net_grace_sec = sec;
    } else {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        if (pwr::net_grace_sec == sec) {
            xSemaphoreGive(pwr::mu);
            return;
        }
        pwr::net_grace_sec = sec;
        xSemaphoreGive(pwr::mu);
    }
    if (xTaskCreate(PersistNetGraceTask, "net_grace_nvs", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(sec)), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(net_grace_nvs) failed");
    }
}

void PowerPolicy::SetStandbyToShutdownSec(int sec)
{
    sec = NormalizeStandbyToShutdownSec(sec);
    if (pwr::mu == nullptr) {
        if (pwr::standby_to_shutdown_sec == sec) {
            return;
        }
        pwr::standby_to_shutdown_sec = sec;
    } else {
        xSemaphoreTake(pwr::mu, portMAX_DELAY);
        if (pwr::standby_to_shutdown_sec == sec) {
            xSemaphoreGive(pwr::mu);
            return;
        }
        pwr::standby_to_shutdown_sec = sec;
        xSemaphoreGive(pwr::mu);
    }
    if (xTaskCreate(PersistStandbyToOffTask, "stb_off_nvs", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(sec)), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(stb_off_nvs) failed");
    }
}
