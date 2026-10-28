#include "settings_test_screens.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "device_state.h"
#include "factory-test-assets/factory_test_assets.h"
#include "fontpack_lvgl.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "metalio_audio.h"
#include "screen_common.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

#include <atomic>
#include <cstdio>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

LV_FONT_DECLARE(font_misans_regular_160_2);


static constexpr const char* TAG = "SettingsTestAging";
static constexpr const char* kScreenId = "settings_test_aging";
/** factory_test_audio ≈2.25s；PlaySound 返回只表示入队，需再等播完 */
static constexpr uint32_t kAudioClipMs = 2500;
/** 播完后再隔 5s 进入下一轮 */
static constexpr uint32_t kAudioGapMs = 5000;
static constexpr uint32_t kAudioReplayAfterMs = kAudioClipMs + kAudioGapMs;
/** 马达：震 0.5s → 停 10s，one-shot 链式循环 */
static constexpr uint32_t kVibeOnMs = 500;
static constexpr uint32_t kVibeOffMs = 10000;
static constexpr lv_coord_t kPad = UiSx(12);

/** 与书库/清单标题一致：内置 fontpack 30@2，失败回退 UI 25@2 */
static const lv_font_t* AgingTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

static lv_obj_t* s_scr = nullptr;
static lv_obj_t* s_counter_lbl = nullptr;
static esp_timer_handle_t s_audio_loop_timer = nullptr;
static esp_timer_handle_t s_vibe_loop_timer = nullptr;

static std::atomic<bool> s_active{false};
static std::atomic<uint32_t> s_session{0};
static std::atomic<bool> s_play_pending{false};
static std::atomic<bool> s_vibe_on{false};

static bool s_holding_testing_state = false;
static bool s_holding_standby_inhibit = false;
static DeviceState s_saved_device_state = kDeviceStateIdle;
static uint32_t s_counter = 0;

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
    ESP_LOGI(TAG, "vibe %s for %lums", on ? "on" : "off", static_cast<unsigned long>(hold_ms));

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
    ScreenLvAsync([](void* p) {
        const uint32_t cycle_session=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
        if(!s_active.load(std::memory_order_acquire) || s_session.load(std::memory_order_acquire)!=cycle_session)return;
        ++s_counter;
        UpdateCounterLabel();
    },reinterpret_cast<void*>(static_cast<uintptr_t>(session)));
    ESP_LOGI(TAG, "audio play; next after ~%lums clip + %lums gap",
             static_cast<unsigned long>(kAudioClipMs), static_cast<unsigned long>(kAudioGapMs));
    // 入队后约 clip 播完，再空 5s，然后下一轮
    ArmNextAudioPlay(session);
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
    // 仅踢第一拍；后续由「播完 + 5s」one-shot 链式循环
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

static void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_active.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    TearDownRuntime();
    s_scr = nullptr;
    s_counter_lbl = nullptr;
    ESP_LOGI(TAG, "aging test screen deleted");
}


bool SettingsTestAgingScreen::IsActive() {
    return s_active.load(std::memory_order_acquire);
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
    s_counter = 0;
    s_counter_lbl = nullptr;
    s_play_pending.store(false, std::memory_order_release);
    s_vibe_on.store(false, std::memory_order_release);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_AGING_RUNNING);
    lv_obj_set_style_text_font(title, AgingTitleFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_counter_lbl = lv_label_create(scr);
    lv_obj_set_width(s_counter_lbl, LV_HOR_RES - 2 * kPad);
    lv_obj_set_height(s_counter_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_counter_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_counter_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_counter_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_counter_lbl, title, LV_ALIGN_OUT_BOTTOM_MID, 0, kPad);
    lv_obj_clear_flag(s_counter_lbl, LV_OBJ_FLAG_CLICKABLE);
    UpdateCounterLabel();

    ScreenSetIsHome(false);
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{SettingsTestAgingScreen::Create});

    s_active.store(true, std::memory_order_release);
    // 震 0.5s → 停 10s 循环；退页 TearDown 停表并关马达
    ApplyVibePhase(true, session);
    AcquireStandbyInhibit();
    EnterAudioTestingState();
    KickAudioInit(session);


    return scr;
}
