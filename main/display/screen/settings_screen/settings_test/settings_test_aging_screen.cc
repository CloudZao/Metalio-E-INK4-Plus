#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "settings_test_screens.h"
#include "settings_test_aging_sd.h"
#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "assets/factory_test_assets.h"
#include "board.h"
#include "device_state.h"
#include "fontpack_lvgl.h"
#include "lv_adapter_display.h"
#include "metalio_audio.h"
#include "power_policy.h"
#include "screen_common.h"
#include "screen_common_nav.h"
#include "settings.h"
#include "settings_common.h"
#include "settings_screen/settings_screen.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

LV_FONT_DECLARE(font_misans_regular_160_2);

static constexpr const char* TAG = "SettingsTestAging";
static constexpr const char* kScreenId = "settings_test_aging";
static constexpr uint32_t kAudioClipMs = 2500; // 工厂音频约 2.25 秒，预留播放完成时间
static constexpr uint32_t kAudioGapMs = 5000; // 播放完成后等待时间
static constexpr uint32_t kAudioReplayAfterMs = kAudioClipMs + kAudioGapMs;
static constexpr uint32_t kVibeOnMs = 500; // 马达单次震动时间
static constexpr uint32_t kVibeOffMs = 10000; // 马达相邻震动的间隔
static constexpr lv_coord_t kPad = UiSx(12);
static constexpr int64_t kRestartIntervalUs = 5LL * 60 * 1000000; // 每轮老化持续五分钟

static lv_obj_t* s_scr = nullptr;
static lv_obj_t* s_counter_lbl = nullptr;
static uint32_t s_counter = 0; // 当前开机周期的音频播放轮次
static esp_timer_handle_t s_audio_loop_timer = nullptr;
static esp_timer_handle_t s_vibe_loop_timer = nullptr;

static std::atomic<bool> s_active{false}; // 页面会话有效，退出保存期间继续驱动取消流程
static std::atomic<uint32_t> s_session{0};
static std::atomic<bool> s_play_pending{false};
static std::atomic<bool> s_vibe_on{false};

static bool s_holding_testing_state = false;
static bool s_holding_standby_inhibit = false;
static DeviceState s_saved_device_state = kDeviceStateIdle;
static std::atomic<int32_t> s_restart_count{-1}; // -1 未运行，0 首次运行，正数为已重启次数
static std::atomic<int32_t> s_saved_restart_count{-1}; // 已成功提交的 NVS 状态
static std::atomic<bool> s_save_pending{false}; // 存储任务至多一个在途写入
static std::atomic<bool> s_reboot_pending{false};
static std::atomic<bool> s_save_failed{false};
static lv_timer_t* s_control_timer = nullptr; // 页面退出后保留到取消状态落盘完成
static int64_t s_restart_at = 0;
static int64_t s_save_retry_at = 0;
static bool s_exit_pending = false;
static bool s_exit_to_home = false;
static bool s_exit_navigation_queued = false;
static bool s_save_error_shown = false;

// 与书库、清单标题一致，内置字体不可用时回退 UI 字体。
static const lv_font_t* AgingTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

static void ClearStatusBindings() {
    // 全屏无顶栏：必须摘掉上一页控件指针，否则 1Hz UpdateStatusBar 写已释放对象搞坏 LVGL
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

static int DigitsPerLine() {
    const lv_font_t* font = &font_misans_regular_160_2;
    lv_point_t sz = {};
    lv_text_get_size(&sz, "0", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int avail = static_cast<int>(LV_HOR_RES) - 2 * kPad;
    if (sz.x <= 0) {
        return 4;
    }
    int n = avail / static_cast<int>(sz.x);
    return n < 1 ? 1 : n;
}

static void FormatWrappedCounter(char* out, size_t out_sz, uint32_t value) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
    const int per_line = DigitsPerLine();
    size_t o = 0;
    for (size_t i = 0; digits[i] != '\0'; ++i) {
        if (i > 0 && (static_cast<int>(i) % per_line) == 0) {
            if (o + 1 >= out_sz) {
                break;
            }
            out[o++] = '\n';
        }
        if (o + 1 >= out_sz) {
            break;
        }
        out[o++] = digits[i];
    }
    out[(o < out_sz) ? o : (out_sz - 1)] = '\0';
}

static void UpdateCounterLabel() {
    if (s_counter_lbl == nullptr || !lv_obj_is_valid(s_counter_lbl)) {
        return;
    }
    char buf[48];
    FormatWrappedCounter(buf, sizeof(buf), s_counter);
    lv_label_set_text(s_counter_lbl, buf);
    // 大字累加用全刷，避免局刷残影叠字
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    lv_obj_invalidate(s_counter_lbl);
    if (s_scr != nullptr && lv_obj_is_valid(s_scr)) {
        lv_obj_invalidate(s_scr);
    }
}

static void EnterAudioTestingState() {
    if (s_holding_testing_state) {
        return;
    }
    auto& app = Application::GetInstance();
    s_saved_device_state = app.GetDeviceState();
    if (s_saved_device_state != kDeviceStateAudioTesting) {
        app.SetDeviceState(kDeviceStateAudioTesting);
    }
    s_holding_testing_state = true;
}

static void LeaveAudioTestingState() {
    if (!s_holding_testing_state) {
        return;
    }
    s_holding_testing_state = false;
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateAudioTesting) {
        return;
    }
    DeviceState restore = s_saved_device_state;
    if (restore == kDeviceStateAudioTesting) {
        restore = kDeviceStateIdle;
    }
    app.SetDeviceState(restore);
}

static void AcquireStandbyInhibit() {
    if (s_holding_standby_inhibit) {
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    s_holding_standby_inhibit = true;
}

static void ReleaseStandbyInhibit() {
    if (!s_holding_standby_inhibit) {
        return;
    }
    s_holding_standby_inhibit = false;
    PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
}

static void StopAudioLoopTimer() {
    if (s_audio_loop_timer != nullptr) {
        esp_timer_stop(s_audio_loop_timer);
        esp_timer_delete(s_audio_loop_timer);
        s_audio_loop_timer = nullptr;
    }
}

static void StopVibeLoopTimer() {
    if (s_vibe_loop_timer != nullptr) {
        esp_timer_stop(s_vibe_loop_timer);
        esp_timer_delete(s_vibe_loop_timer);
        s_vibe_loop_timer = nullptr;
    }
}

static void ApplyVibePhase(bool on, uint32_t session);

static void OnVibeLoopEspTimer(void* arg) {
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    // 当前相位结束 → 切到另一相位
    ApplyVibePhase(!s_vibe_on.load(std::memory_order_acquire), session);
}

static void EnsureVibeLoopTimer(uint32_t session) {
    if (s_vibe_loop_timer != nullptr) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &OnVibeLoopEspTimer,
        .arg = reinterpret_cast<void*>(static_cast<uintptr_t>(session)),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "aging_vibe",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &s_vibe_loop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vibe loop timer create: %s", esp_err_to_name(err));
        s_vibe_loop_timer = nullptr;
    }
}

static void ApplyVibePhase(bool on, uint32_t session) {
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    s_vibe_on.store(on, std::memory_order_release);
    Board::GetInstance().SetVibration(on);
    const uint32_t hold_ms = on ? kVibeOnMs : kVibeOffMs;
    ESP_LOGD(TAG, "vibe %s for %lums", on ? "on" : "off", static_cast<unsigned long>(hold_ms));

    EnsureVibeLoopTimer(session);
    if (s_vibe_loop_timer == nullptr) {
        return;
    }
    esp_timer_stop(s_vibe_loop_timer);
    const esp_err_t err =
        esp_timer_start_once(s_vibe_loop_timer, static_cast<uint64_t>(hold_ms) * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "vibe loop arm failed: %s", esp_err_to_name(err));
    }
}

static void ReleaseOwnedAudioIfNeeded() {
    auto& app = Application::GetInstance();
    app.RequestAudioServiceRelease(Application::AudioClient::AgingTest);
}

static void SchedulePlay(uint32_t session);

static void OnAudioLoopEspTimer(void* arg) {
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    SchedulePlay(session);
}

static void EnsureAudioLoopTimer(uint32_t session) {
    if (s_audio_loop_timer != nullptr) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &OnAudioLoopEspTimer,
        .arg = reinterpret_cast<void*>(static_cast<uintptr_t>(session)),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "aging_aud",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &s_audio_loop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio loop timer create: %s", esp_err_to_name(err));
        s_audio_loop_timer = nullptr;
    }
}

static void ArmNextAudioPlay(uint32_t session) {
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    EnsureAudioLoopTimer(session);
    if (s_audio_loop_timer == nullptr) {
        return;
    }
    esp_timer_stop(s_audio_loop_timer);
    const esp_err_t err =
        esp_timer_start_once(s_audio_loop_timer, static_cast<uint64_t>(kAudioReplayAfterMs) * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio loop arm failed: %s", esp_err_to_name(err));
    }
}

static void PlayFactoryAudioOnTask(uint32_t session) {
    struct Guard {
        uint32_t session; // 播放请求所属会话
        ~Guard() {
            if (s_session.load(std::memory_order_acquire) == session) {
                s_play_pending.store(false, std::memory_order_release);
            }
        }
    } guard{session};

    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        ESP_LOGE(TAG, "audio loop stopped: service not started session=%lu", static_cast<unsigned long>(session));
        return;
    }
    as.PlaySound(FactoryTestAssets::OGG_FACTORY_TEST_AUDIO);
    if (!ScreenLvAsync([](void* context) {
        const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
        if (!s_active.load() || s_session.load() != session) return;
        ++s_counter;
        UpdateCounterLabel();
        SettingsTestAgingSd_ReadCycle(ArmNextAudioPlay, session);
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(session)))) {
        ESP_LOGW(TAG, "aging cycle counter update unavailable");
        ArmNextAudioPlay(session);
    }
    ESP_LOGD(TAG, "audio play; after SD read wait ~%lums clip + %lums gap",
             static_cast<unsigned long>(kAudioClipMs), static_cast<unsigned long>(kAudioGapMs));
    // SD 读取结果回收后，再等待播放完成及间隔，避免积压多轮解码。
}

static void SchedulePlay(uint32_t session) {
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    bool expected = false;
    if (!s_play_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!Application::GetInstance().Schedule([session]() { PlayFactoryAudioOnTask(session); }, Application::TaskDomain::Audio)) {
        s_play_pending.store(false);
    }
}

static void AsyncArmAudioLoop(void* p) {
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    if (!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire) != session) {
        return;
    }
    // 仅启动首轮；后续由 SD 结果回收和音频间隔定时器串行调度
    SchedulePlay(session);
}

static void InitializeAudio(uint32_t session) {
    auto& app = Application::GetInstance();
    bool now_running = false;
    if (MetalioAudio_WaitModeReady(kMetalioAudioInitWaitMs, [session] {
            return s_session.load() == session && s_active.load();
        })) {
        now_running = app.AcquireAudioService(Application::AudioClient::AgingTest, [session] {
            return s_session.load(std::memory_order_acquire) == session && s_active.load(std::memory_order_acquire);
        });
    }

    if (s_session.load(std::memory_order_acquire) == session && s_active.load(std::memory_order_acquire) &&
        now_running) {
        if (!ScreenLvAsync(AsyncArmAudioLoop,
                                      reinterpret_cast<void*>(static_cast<uintptr_t>(session)))) {
            ESP_LOGW(TAG, "PostLvAsync arm audio failed");
        }
    }
}

static void KickAudioInit(uint32_t session) {
    if (!Application::GetInstance().Schedule([session]() { InitializeAudio(session); }, Application::TaskDomain::Audio)) {
        ESP_LOGW(TAG, "audio init task create failed");
    }
}

static void TearDownRuntime() {
    SettingsTestAgingSd_Reset();
    StopAudioLoopTimer();
    StopVibeLoopTimer();
    s_play_pending.store(false, std::memory_order_release);
    s_vibe_on.store(false, std::memory_order_release);
    Board::GetInstance().SetVibration(false);

    auto& as = Application::GetInstance().GetAudioService();
    if (as.IsStarted()) {
        as.ResetDecoder();
    }

    LeaveAudioTestingState();
    ReleaseStandbyInhibit();
    ReleaseOwnedAudioIfNeeded();
}

static void PollAgingControl(lv_timer_t*) {
    const int64_t now = esp_timer_get_time();
    const int32_t desired = s_restart_count.load();
    if (desired != s_saved_restart_count.load() && now >= s_save_retry_at && !s_save_pending.exchange(true)) {
        s_save_retry_at = now + 5LL * 1000000;
        if (!Application::GetInstance().Schedule([] {
            const int32_t value = s_restart_count.load();
            Settings settings("factory_test", true);
            if (settings.SetInt("aging_restarts", value) && settings.Commit()) {
                s_saved_restart_count.store(value);
                s_save_failed.store(false);
                ESP_LOGI(TAG, "aging state saved restart_count=%ld", static_cast<long>(value));
            } else if (!s_save_failed.exchange(true)) {
                ESP_LOGE(TAG, "aging state save failed restart_count=%ld", static_cast<long>(value));
            }
            s_save_pending.store(false);
        }, Application::TaskDomain::Storage)) {
            s_save_pending.store(false);
            if (!s_save_failed.exchange(true)) ESP_LOGE(TAG, "aging state save dispatch failed; retry retained");
        }
    }
    const bool saved = !s_save_pending.load() && s_restart_count.load() == s_saved_restart_count.load();
    if (!s_active.load()) {
        if (saved && s_control_timer) {
            lv_timer_delete(s_control_timer);
            s_control_timer = nullptr;
        }
        return;
    }
    if (s_save_failed.load() && !s_save_error_shown) {
        ScreenShowToast(Lang::Strings::SETTINGS_TEST_AGING_SAVE_FAILED);
        s_save_error_shown = true;
    }
    if (saved) s_save_error_shown = false;
    if (s_exit_pending) {
        if (saved && !s_exit_navigation_queued) {
            s_exit_navigation_queued = true;
            if (s_exit_to_home) ScreenGoHome();
            else ScreenNavigateBack();
        }
        return;
    }
    if (!saved || desired < 0 || now < s_restart_at) return;
    if (!SettingsTestAgingSd_StopReading() || s_reboot_pending.exchange(true)) return;
    const uint32_t session = s_session.load();
    if (!Application::GetInstance().Schedule([session] {
        if (s_active.load() && s_session.load() == session && s_restart_count.load() >= 0) {
            Application::GetInstance().Reboot();
        }
        s_reboot_pending.store(false);
    })) {
        s_reboot_pending.store(false);
        s_restart_at = now + 5LL * 1000000;
        ESP_LOGE(TAG, "aging reboot dispatch failed; retry retained");
    }
}

static void RequestAgingExit(bool home = false) {
    if (s_exit_pending) {
        s_exit_navigation_queued = false;
        PollAgingControl(nullptr);
        return;
    }
    s_exit_pending = true;
    s_exit_to_home = home;
    s_restart_count.store(-1);
    s_save_retry_at = 0;
    s_session.fetch_add(1, std::memory_order_acq_rel);
    // 立即停止老化负载，但继续保留页面，直到取消状态提交成功。
    TearDownRuntime();
    PollAgingControl(nullptr);
}

static bool OnAgingKey(const char* key) {
    if (std::strcmp(key, "vk_home") != 0 && std::strcmp(key, "vk_prev") != 0) return false;
    RequestAgingExit();
    return true;
}

static bool OnAgingLongKey(const char* key) {
    if (std::strcmp(key, "vk_home") != 0) return false;
    RequestAgingExit(true);
    return true;
}

static void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_active.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    TearDownRuntime();
    s_restart_count.store(-1);
    s_save_retry_at = 0;
    PollAgingControl(nullptr);
    s_scr = nullptr;
    s_counter_lbl = nullptr;
    ESP_LOGI(TAG, "aging test screen deleted");
}

bool SettingsTestAgingScreen::LoadBootState() {
    Settings settings("factory_test", false);
    const int32_t saved = settings.GetInt("aging_restarts", -1);
    s_saved_restart_count.store(saved);
    s_restart_count.store(saved >= 0 ? (saved < INT32_MAX ? saved + 1 : INT32_MAX) : -1);
    return saved >= 0;
}

lv_obj_t* SettingsTestAgingScreen::CreateBootResume() {
    ScreenCommon_ClearBackStack();
    ScreenCommon_PushBackFactory(SettingsScreen::CreateTest);
    return Create();
}

bool SettingsTestAgingScreen::IsActive() {
    return s_active.load(std::memory_order_acquire) && s_restart_count.load() >= 0;
}

lv_obj_t* SettingsTestAgingScreen::Create() {
    ESP_LOGI(TAG, "create aging test screen");

    if (s_active.load(std::memory_order_acquire) || s_holding_testing_state || s_holding_standby_inhibit ||
        s_audio_loop_timer != nullptr || s_vibe_loop_timer != nullptr) {
        ESP_LOGW(TAG, "create while previous session live — force release");
        s_active.store(false, std::memory_order_release);
        TearDownRuntime();
    }

    ClearStatusBindings();

    const uint32_t session = s_session.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (s_restart_count.load() < 0) s_restart_count.store(0);
    s_restart_at = esp_timer_get_time() + kRestartIntervalUs;
    s_save_retry_at = 0;
    s_exit_pending = false;
    s_exit_to_home = false;
    s_exit_navigation_queued = false;
    s_save_error_shown = false;
    s_reboot_pending.store(false);
    s_counter = 0;
    s_counter_lbl = nullptr;
    s_play_pending.store(false, std::memory_order_release);
    s_vibe_on.store(false, std::memory_order_release);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_AGING_RUNNING);
    lv_obj_set_style_text_font(title, AgingTitleFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    auto* count_title = lv_label_create(scr);
    lv_label_set_text_fmt(count_title, Lang::Strings::SETTINGS_TEST_AGING_RESTART_COUNT_FMT,
                          static_cast<int>(s_restart_count.load()));
    lv_obj_set_style_text_font(count_title, AgingTitleFont(), 0);
    lv_obj_align_to(count_title, title, LV_ALIGN_OUT_BOTTOM_MID, 0, kPad);
    s_counter_lbl = lv_label_create(scr);
    lv_obj_set_width(s_counter_lbl, LV_HOR_RES - 2 * kPad);
    lv_obj_set_height(s_counter_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_counter_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_counter_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_counter_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_counter_lbl, count_title, LV_ALIGN_OUT_BOTTOM_MID, 0, kPad);
    lv_obj_clear_flag(s_counter_lbl, LV_OBJ_FLAG_CLICKABLE);
    UpdateCounterLabel();

    const lv_coord_t wallpaper_side = std::min<lv_coord_t>(LV_HOR_RES - 2 * UiSx(24), LV_VER_RES / 3);
    auto* wallpaper = SettingsTestAgingSd_Build(scr, wallpaper_side);
    lv_obj_center(wallpaper);
    auto* back = SettingsCreateSelectableOption(scr, Lang::Strings::SETTINGS_TEST_BACK,
        [](lv_event_t*) { RequestAgingExit(); }, 0);
    lv_obj_set_size(back, UiSx(200), UiSy(60));
    const lv_coord_t wallpaper_bottom = (LV_VER_RES + wallpaper_side) / 2;
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, (wallpaper_bottom + LV_VER_RES - UiSy(60)) / 2);
    ScreenSetIsHome(false);
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{.factory = SettingsTestAgingScreen::Create,
        .on_key = OnAgingKey, .on_key_long_press = OnAgingLongKey});

    s_active.store(true, std::memory_order_release);
    // 震 0.5s → 停 10s 循环；退页 TearDown 停表并关马达
    ApplyVibePhase(true, session);
    AcquireStandbyInhibit();
    EnterAudioTestingState();
    KickAudioInit(session);
    if (!s_control_timer) s_control_timer = lv_timer_create(PollAgingControl, 1000, nullptr);
    if (!s_control_timer) ESP_LOGE(TAG, "aging control timer creation failed");
    PollAgingControl(nullptr);

    return scr;
}
