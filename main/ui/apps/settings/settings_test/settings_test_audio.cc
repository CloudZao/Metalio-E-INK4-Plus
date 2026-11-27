#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sysmgmt/audio/audio_runtime.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/settings/settings_test/settings_test_audio.h"
#include "assets/lang_config.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "infra/audio/audio_service.h"
#include "sysmgmt/device_state.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/runtime/modal.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr const char* kTag = "SettingsTestAudio";
static constexpr const char* kIconPass = "A:ic_app_test_pass.spng";
static constexpr const char* kIconFail = "A:ic_app_test_fail.spng";
static constexpr uint32_t kMaxRecordMs = 5000;
static constexpr uint32_t kPlaybackMarginMs = 500;
static constexpr uint32_t kConfirmDelayMs = 1000;
static constexpr lv_coord_t kRowH = UiSy(44);
static constexpr lv_coord_t kIconSz = UiSx(28);

enum class SettingsTestAudioState {
    Idle,
    Recording,
    Playing,
};

static SettingsTestAudioState s_state = SettingsTestAudioState::Idle;
static std::atomic<bool> s_alive{false};
static bool s_holding_testing_state = false;
static bool s_press_held = false;
static int64_t s_record_start_us = 0;

static lv_obj_t* s_root_scr = nullptr;
static lv_obj_t* s_row = nullptr;
static lv_obj_t* s_icon = nullptr;
static lv_obj_t* s_record_btn = nullptr;
static lv_obj_t* s_record_lbl = nullptr;
static lv_obj_t* s_confirm_mask = nullptr;
static lv_timer_t* s_max_record_timer = nullptr;
static lv_timer_t* s_playback_timer = nullptr;
static lv_timer_t* s_confirm_timer = nullptr;

static std::atomic<bool> s_audio_ready{false};
static std::atomic<bool> s_audio_init_pending{false};  // 录音测试初始化消息在途标记

static void UpdateButtonUi();
static void KickAudioInitIfNeeded();
static void LeaveAudioTestingState();
static void StartRecording();

static UiModal s_window_modal;

static bool IsLive() {
    return s_alive && s_root_scr != nullptr;
}

static void StopConfirmTimer() {
    if (s_confirm_timer != nullptr) {
        lv_timer_delete(s_confirm_timer);
        s_confirm_timer = nullptr;
    }
}

static void CloseConfirmDialog() {
    if (s_confirm_mask != nullptr && IsLive()) {
        lv_obj_delete(s_confirm_mask);
    }
    s_confirm_mask = nullptr;
}

static void SetPassFail(bool pass) {
    if (!IsLive() || s_icon == nullptr) {
        return;
    }
    lv_image_set_src(s_icon, pass ? kIconPass : kIconFail);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
}

static void OnAudioConfirmResult(bool pass) {
    if (!IsLive()) {
        return;
    }
    SetPassFail(pass);
    ESP_LOGI(kTag, "user confirm audio: %s", pass ? "pass" : "fail");
}

static void OnConfirmYesClicked(lv_event_t* /*e*/) {
    CloseConfirmDialog();
    OnAudioConfirmResult(true);
}

static void OnConfirmNoClicked(lv_event_t* /*e*/) {
    CloseConfirmDialog();
    OnAudioConfirmResult(false);
}

static void ShowConfirmDialog() {
    if (!IsLive() || s_root_scr == nullptr || s_confirm_mask != nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(s_root_scr);
    s_confirm_mask = mask;
    s_window_modal.Bind(mask, [](void*) { CloseConfirmDialog(); });
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(80), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, UiSy(144), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_border_width(card, kScreenBorderW, 0);
    lv_obj_set_style_radius(card, UiSx(10), 0);
    lv_obj_set_style_pad_all(card, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, UiSy(20), 0);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* msg = lv_label_create(card);
    lv_label_set_text(msg, LangString_SETTINGS_TEST_AUDIO_CONFIRM);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, UiFont(), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, UiSy(48));
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);

    auto make_btn = [&](const char* text, lv_event_cb_t cb) {
        auto* btn = ScreenCreateSelectableOption(btn_row, text, cb, 0);
        lv_obj_set_width(btn, UiSx(120));
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_min_height(btn, UiSy(44), 0);
    };
    make_btn(LangString_SETTINGS_TEST_YES, OnConfirmYesClicked);
    make_btn(LangString_SETTINGS_TEST_NO, OnConfirmNoClicked);
}

static void OnConfirmTimer(lv_timer_t* /*t*/) {
    s_confirm_timer = nullptr;
    if (!IsLive()) {
        return;
    }
    ShowConfirmDialog();
}

static void ScheduleConfirmDialog() {
    StopConfirmTimer();
    s_confirm_timer = lv_timer_create(OnConfirmTimer, kConfirmDelayMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
}

static void StopPlaybackTimer() {
    if (s_playback_timer != nullptr) {
        lv_timer_delete(s_playback_timer);
        s_playback_timer = nullptr;
    }
}

static void FinishPlaybackUi() {
    if (s_state != SettingsTestAudioState::Playing || !IsLive()) {
        return;
    }
    StopPlaybackTimer();
    LeaveAudioTestingState();
    s_state = SettingsTestAudioState::Idle;
    UpdateButtonUi();
    ScheduleConfirmDialog();
    ESP_LOGI(kTag, "playback finished");
}

static void OnPlaybackTimer(lv_timer_t* /*t*/) {
    s_playback_timer = nullptr;
    FinishPlaybackUi();
}

static void SchedulePlaybackUiReset() {
    const int64_t now = esp_timer_get_time();
    int duration_ms = static_cast<int>((now - s_record_start_us) / 1000);
    if (duration_ms < 1) {
        duration_ms = 1;
    }
    const uint32_t wait_ms = static_cast<uint32_t>(duration_ms) + kPlaybackMarginMs;

    StopPlaybackTimer();
    s_playback_timer = lv_timer_create(OnPlaybackTimer, wait_ms, nullptr);
    lv_timer_set_repeat_count(s_playback_timer, 1);
}

// PowerPolicy 仅在 Speaking / AudioTesting 时开 PA；自检只走 EnableAudioTesting，需同步 DeviceState。
static void EnterAudioTestingState() {
    if (s_holding_testing_state) {
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::AudioTesting);
    s_holding_testing_state = true;
}

static void LeaveAudioTestingState() {
    if (!s_holding_testing_state) {
        return;
    }
    s_holding_testing_state = false;
    PowerPolicy::GetInstance().Release(PowerNeed::AudioTesting);
}

static void UpdateButtonUi() {
    if (!s_alive || s_row == nullptr || s_record_lbl == nullptr) {
        return;
    }

    switch (s_state) {
        case SettingsTestAudioState::Idle:
            lv_label_set_text(s_record_lbl, LangString_SETTINGS_TEST_AUDIO_HOLD);
            lv_obj_add_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
            break;
        case SettingsTestAudioState::Recording:
            lv_label_set_text(s_record_lbl, LangString_SETTINGS_TEST_AUDIO_RECORDING);
            lv_obj_add_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
            break;
        case SettingsTestAudioState::Playing:
            lv_label_set_text(s_record_lbl, LangString_RECORD_PLAYING);
            lv_obj_remove_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
            break;
    }
}

static void StopMaxRecordTimer() {
    if (s_max_record_timer != nullptr) {
        lv_timer_delete(s_max_record_timer);
        s_max_record_timer = nullptr;
    }
}

static void StopRecordingAndPlay() {
    if (s_state != SettingsTestAudioState::Recording) {
        return;
    }

    StopMaxRecordTimer();
    auto& as = AudioRuntime::GetInstance().Service();
    as.EnableAudioTesting(false);
    s_state = SettingsTestAudioState::Playing;
    UpdateButtonUi();
    SchedulePlaybackUiReset();
    ESP_LOGI(kTag, "record stopped, playback started");
}

static void OnMaxRecordTimer(lv_timer_t* /*t*/) {
    s_max_record_timer = nullptr;
    ESP_LOGI(kTag, "max record duration reached (%lums)", static_cast<unsigned long>(kMaxRecordMs));
    StopRecordingAndPlay();
}

static void StartRecording() {
    if (s_state != SettingsTestAudioState::Idle || !IsLive()) {
        return;
    }

    StopConfirmTimer();
    CloseConfirmDialog();

    auto& as = AudioRuntime::GetInstance().Service();
    if (!as.IsStarted() || !BluetoothAudio_ModeReady()) {
        // 等 AudioInitTask；手指仍按住则就绪后补开录
        ESP_LOGI(kTag, "audio not ready, defer record while held=%d", s_press_held ? 1 : 0);
        KickAudioInitIfNeeded();
        return;
    }
    if (as.IsVoiceCaptureRunning()) {
        ESP_LOGW(kTag, "audio processor busy, skip record");
        return;
    }

    EnterAudioTestingState();
    as.ResetDecoder();
    as.EnableAudioTesting(true);

    s_record_start_us = esp_timer_get_time();
    s_state = SettingsTestAudioState::Recording;
    UpdateButtonUi();
    StopMaxRecordTimer();
    s_max_record_timer = lv_timer_create(OnMaxRecordTimer, kMaxRecordMs, nullptr);
    lv_timer_set_repeat_count(s_max_record_timer, 1);
    ESP_LOGI(kTag, "recording started");
}

static void OnRecordPressed(lv_event_t* /*e*/) {
    s_press_held = true;
    KickAudioInitIfNeeded();
    StartRecording();
}

static void OnRecordReleased(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    s_press_held = false;
    if (s_state == SettingsTestAudioState::Recording) {
        StopRecordingAndPlay();
    }
}

static void ForceStopWithoutPlayback() {
    s_press_held = false;
    StopMaxRecordTimer();
    StopPlaybackTimer();
    StopConfirmTimer();
    CloseConfirmDialog();

    auto& as = AudioRuntime::GetInstance().Service();
    if (s_state == SettingsTestAudioState::Recording) {
        as.ResetDecoder();
        as.EnableAudioTesting(false);
    } else if (s_state == SettingsTestAudioState::Playing) {
        as.ResetDecoder();
    }
    LeaveAudioTestingState();
    s_state = SettingsTestAudioState::Idle;
}

static void AsyncOnAudioReady(void* /*p*/) {
    if (!s_alive) {
        return;
    }
    UpdateButtonUi();
    // 进页预热后仍可能首按撞上未就绪：手指未抬则补开录
    if (s_press_held && s_audio_ready.load()) {
        StartRecording();
    }
}

static void InitializeAudio() {
    {
        PowerNeedHold power(PowerNeed::LocalMic);
        if (BluetoothAudio_WaitModeReady(kBluetoothAudioInitWaitMs, [] { return s_alive.load(); }) && s_alive.load()) {
            AudioRuntime::GetInstance().Acquire(AudioClient::FactoryTest, [] { return s_alive.load(); });
        }
    }
    s_audio_ready.store(AudioRuntime::GetInstance().Service().IsStarted() && BluetoothAudio_ModeReady());
    s_audio_init_pending.store(false);
    if (s_alive.load()) {
        if (!ScreenLvAsync(AsyncOnAudioReady, nullptr)) {
            ESP_LOGW(kTag, "ScreenLvAsync for audio ready failed");
        }
    }
}

static void KickAudioInitIfNeeded() {
    if (s_audio_ready.load() && AudioRuntime::GetInstance().Service().IsStarted() && BluetoothAudio_ModeReady()) {
        return;
    }
    if (s_audio_init_pending.exchange(true)) {
        return;
    }
    s_audio_ready.store(false);
    if (!TaskExecutor::Schedule(InitializeAudio, TaskDomain::Audio)) {
        s_audio_init_pending.store(false);
        ESP_LOGE(kTag, "audio init task create failed");
    }
}

void SettingsTestAudio_BuildRow(lv_obj_t* parent, lv_obj_t* root_scr) {
    s_root_scr = root_scr;

    lv_obj_t* row = lv_obj_create(parent);
    s_row = row;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, OnRecordPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(row, OnRecordReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(row, OnRecordReleased, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_t* title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, LangString_SETTINGS_TEST_AUDIO_TITLE);
    lv_obj_set_width(title_lbl, UiSx(88));  // 容纳英文 Audio
    lv_obj_set_height(title_lbl, lv_font_get_line_height(UiFont()));
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_lbl, UiFont(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    s_icon = lv_image_create(row);
    lv_obj_set_size(s_icon, kIconSz, kIconSz);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    s_record_btn = lv_obj_create(row);
    lv_obj_remove_style_all(s_record_btn);
    lv_obj_set_size(s_record_btn, UiSx(152), UiSy(40));  // 容纳英文 Hold to talk
    lv_obj_set_style_bg_color(s_record_btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_record_btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(s_record_btn, kScreenBorderW, 0);
    lv_obj_set_style_radius(s_record_btn, kScreenOptionRadius, 0);
    lv_obj_clear_flag(s_record_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, LangString_SETTINGS_TEST_AUDIO_HOLD);
    lv_obj_set_width(s_record_lbl, UiSx(140));
    lv_label_set_long_mode(s_record_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_record_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_record_lbl, UiFont(), 0);
    lv_obj_center(s_record_lbl);
    lv_obj_clear_flag(s_record_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void SettingsTestAudio_OnLoad() {
    s_alive = true;
    s_state = SettingsTestAudioState::Idle;
    s_press_held = false;
    KickAudioInitIfNeeded();
    UpdateButtonUi();
}

void SettingsTestAudio_Teardown() {
    s_alive = false;
    ForceStopWithoutPlayback();
    s_root_scr = nullptr;
    s_row = nullptr;
    s_icon = nullptr;
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_confirm_mask = nullptr;
    s_audio_ready.store(false);
    AudioRuntime::GetInstance().RequestRelease(AudioClient::FactoryTest);
}
