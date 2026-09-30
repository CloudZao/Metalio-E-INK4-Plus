#include "settings_test_audio.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "device_state.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "settings_common.h"
#include "ui_scale.h"

#include <atomic>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* kTag = "SettingsTestAudio";
constexpr const char* kIconPass = "A:ic_app_test_pass.spng";
constexpr const char* kIconFail = "A:ic_app_test_fail.spng";
constexpr uint32_t kMaxRecordMs = 5000;
constexpr uint32_t kPlaybackMarginMs = 500;
constexpr uint32_t kConfirmDelayMs = 1000;
constexpr lv_coord_t kRowH = UiSy(56);
constexpr lv_coord_t kIconSz = UiSx(28);

enum class State {
    Idle,
    Recording,
    Playing,
};

State s_state = State::Idle;
bool s_alive = false;
bool s_wake_disabled = false;
bool s_holding_testing_state = false;
bool s_press_held = false;
DeviceState s_saved_device_state = kDeviceStateIdle;
int64_t s_record_start_us = 0;

lv_obj_t* s_root_scr = nullptr;
lv_obj_t* s_row = nullptr;
lv_obj_t* s_icon = nullptr;
lv_obj_t* s_record_btn = nullptr;
lv_obj_t* s_record_lbl = nullptr;
lv_obj_t* s_confirm_mask = nullptr;
lv_timer_t* s_max_record_timer = nullptr;
lv_timer_t* s_playback_timer = nullptr;
lv_timer_t* s_confirm_timer = nullptr;

std::atomic<bool> s_audio_ready{false};
TaskHandle_t s_audio_init_task = nullptr;

void UpdateButtonUi();
void KickAudioInitIfNeeded();
void LeaveAudioTestingState();
void StartRecording();

bool IsLive() {
    return s_alive && s_root_scr != nullptr;
}

void StopConfirmTimer() {
    if (s_confirm_timer != nullptr) {
        lv_timer_delete(s_confirm_timer);
        s_confirm_timer = nullptr;
    }
}

void CloseConfirmDialog() {
    if (s_confirm_mask != nullptr && IsLive()) {
        lv_obj_delete(s_confirm_mask);
    }
    s_confirm_mask = nullptr;
}

void SetPassFail(bool pass) {
    if (!IsLive() || s_icon == nullptr) {
        return;
    }
    lv_image_set_src(s_icon, pass ? kIconPass : kIconFail);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
}

void OnAudioConfirmResult(bool pass) {
    if (!IsLive()) {
        return;
    }
    SetPassFail(pass);
    ESP_LOGI(kTag, "user confirm audio: %s", pass ? "pass" : "fail");
}

void OnConfirmYesClicked(lv_event_t* /*e*/) {
    CloseConfirmDialog();
    OnAudioConfirmResult(true);
}

void OnConfirmNoClicked(lv_event_t* /*e*/) {
    CloseConfirmDialog();
    OnAudioConfirmResult(false);
}

void ShowConfirmDialog() {
    if (!IsLive() || s_root_scr == nullptr || s_confirm_mask != nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(s_root_scr);
    s_confirm_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(48), UiSy(220));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kSettingsBorderW, 0);
    lv_obj_set_style_radius(card, UiSx(10), 0);
    lv_obj_set_style_pad_all(card, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, kSettingsOptionGap, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* msg = lv_label_create(card);
    lv_label_set_text(msg, Lang::Strings::SETTINGS_TEST_AUDIO_CONFIRM);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, UiSy(48));
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);

    auto make_btn = [&](const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, UiSx(120), UiSy(44));
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, kSettingsBorderW, 0);
        lv_obj_set_style_radius(btn, kSettingsOptionRadius, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };
    make_btn(Lang::Strings::SETTINGS_TEST_YES, OnConfirmYesClicked);
    make_btn(Lang::Strings::SETTINGS_TEST_NO, OnConfirmNoClicked);
}

void OnConfirmTimer(lv_timer_t* /*t*/) {
    s_confirm_timer = nullptr;
    if (!IsLive()) {
        return;
    }
    ShowConfirmDialog();
}

void ScheduleConfirmDialog() {
    StopConfirmTimer();
    s_confirm_timer = lv_timer_create(OnConfirmTimer, kConfirmDelayMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
}

void StopPlaybackTimer() {
    if (s_playback_timer != nullptr) {
        lv_timer_delete(s_playback_timer);
        s_playback_timer = nullptr;
    }
}

void FinishPlaybackUi() {
    if (s_state != State::Playing || !IsLive()) {
        return;
    }
    StopPlaybackTimer();
    LeaveAudioTestingState();
    s_state = State::Idle;
    UpdateButtonUi();
    ScheduleConfirmDialog();
    ESP_LOGI(kTag, "playback finished");
}

void OnPlaybackTimer(lv_timer_t* /*t*/) {
    s_playback_timer = nullptr;
    FinishPlaybackUi();
}

void SchedulePlaybackUiReset() {
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

void RestoreWakeWord() {
    if (!s_wake_disabled) {
        return;
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    s_wake_disabled = false;
}

void DisableWakeWordIfNeeded() {
    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsWakeWordRunning()) {
        return;
    }
    as.EnableWakeWordDetection(false);
    s_wake_disabled = true;
}

// PowerPolicy 仅在 Speaking / AudioTesting 时开 PA；自检只走 EnableAudioTesting，需同步 DeviceState。
void EnterAudioTestingState() {
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

void LeaveAudioTestingState() {
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

void UpdateButtonUi() {
    if (!s_alive || s_row == nullptr || s_record_lbl == nullptr) {
        return;
    }

    switch (s_state) {
    case State::Idle:
        lv_label_set_text(s_record_lbl, Lang::Strings::SETTINGS_TEST_AUDIO_HOLD);
        lv_obj_add_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
        break;
    case State::Recording:
        lv_label_set_text(s_record_lbl, Lang::Strings::SETTINGS_TEST_AUDIO_RECORDING);
        lv_obj_add_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
        break;
    case State::Playing:
        lv_label_set_text(s_record_lbl, Lang::Strings::RECORD_PLAYING);
        lv_obj_remove_flag(s_row, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
}

void StopMaxRecordTimer() {
    if (s_max_record_timer != nullptr) {
        lv_timer_delete(s_max_record_timer);
        s_max_record_timer = nullptr;
    }
}

void StopRecordingAndPlay() {
    if (s_state != State::Recording) {
        return;
    }

    StopMaxRecordTimer();
    auto& as = Application::GetInstance().GetAudioService();
    as.EnableAudioTesting(false);
    RestoreWakeWord();
    s_state = State::Playing;
    UpdateButtonUi();
    SchedulePlaybackUiReset();
    ESP_LOGI(kTag, "record stopped, playback started");
}

void OnMaxRecordTimer(lv_timer_t* /*t*/) {
    s_max_record_timer = nullptr;
    ESP_LOGI(kTag, "max record duration reached (%lums)", static_cast<unsigned long>(kMaxRecordMs));
    StopRecordingAndPlay();
}

void StartRecording() {
    if (s_state != State::Idle || !IsLive()) {
        return;
    }

    StopConfirmTimer();
    CloseConfirmDialog();

    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        // 等 AudioInitTask；手指仍按住则就绪后补开录
        ESP_LOGI(kTag, "audio not ready, defer record while held=%d", s_press_held ? 1 : 0);
        KickAudioInitIfNeeded();
        return;
    }
    if (as.IsAudioProcessorRunning()) {
        ESP_LOGW(kTag, "audio processor busy, skip record");
        return;
    }

    DisableWakeWordIfNeeded();
    EnterAudioTestingState();
    as.ResetDecoder();
    as.EnableAudioTesting(true);

    s_record_start_us = esp_timer_get_time();
    s_state = State::Recording;
    UpdateButtonUi();
    StopMaxRecordTimer();
    s_max_record_timer = lv_timer_create(OnMaxRecordTimer, kMaxRecordMs, nullptr);
    lv_timer_set_repeat_count(s_max_record_timer, 1);
    ESP_LOGI(kTag, "recording started");
}

void OnRecordPressed(lv_event_t* /*e*/) {
    s_press_held = true;
    KickAudioInitIfNeeded();
    StartRecording();
}

void OnRecordReleased(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    s_press_held = false;
    if (s_state == State::Recording) {
        StopRecordingAndPlay();
    }
}

void ForceStopWithoutPlayback() {
    s_press_held = false;
    StopMaxRecordTimer();
    StopPlaybackTimer();
    StopConfirmTimer();
    CloseConfirmDialog();

    auto& as = Application::GetInstance().GetAudioService();
    if (s_state == State::Recording) {
        as.ResetDecoder();
        as.EnableAudioTesting(false);
    } else if (s_state == State::Playing) {
        as.ResetDecoder();
    }
    LeaveAudioTestingState();
    RestoreWakeWord();
    s_state = State::Idle;
}

void AsyncOnAudioReady(void* /*p*/) {
    if (!s_alive) {
        return;
    }
    UpdateButtonUi();
    // 进页预热后仍可能首按撞上未就绪：手指未抬则补开录
    if (s_press_held) {
        StartRecording();
    }
}

void AudioInitTask(void* /*arg*/) {
    Application::GetInstance().EnsureAudioServiceRunning();
    s_audio_ready.store(Application::GetInstance().GetAudioService().IsStarted());
    s_audio_init_task = nullptr;
    if (IsLive()) {
        if (!ScreenLvAsync(AsyncOnAudioReady, nullptr)) {
            ESP_LOGW(kTag, "ScreenLvAsync for audio ready failed");
        }
    }
    vTaskDelete(nullptr);
}

void KickAudioInitIfNeeded() {
    if (s_audio_ready.load() &&
        Application::GetInstance().GetAudioService().IsStarted()) {
        return;
    }
    if (s_audio_init_task != nullptr) {
        return;
    }
    s_audio_ready.store(false);
    if (xTaskCreatePinnedToCore(AudioInitTask, "stest_audio", 8 * 1024, nullptr, 5,
                                &s_audio_init_task, 0) != pdPASS) {
        s_audio_init_task = nullptr;
        ESP_LOGE(kTag, "audio init task create failed");
    }
}

}  // namespace

void SettingsTestAudio_BuildRow(lv_obj_t* parent, lv_obj_t* root_scr) {
    s_root_scr = root_scr;

    lv_obj_t* row = lv_obj_create(parent);
    s_row = row;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, OnRecordPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(row, OnRecordReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(row, OnRecordReleased, LV_EVENT_PRESS_LOST, nullptr);

    s_icon = lv_image_create(row);
    lv_obj_set_size(s_icon, kIconSz, kIconSz);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, Lang::Strings::SETTINGS_TEST_AUDIO_TITLE);
    lv_obj_set_width(title_lbl, UiSx(88)); // 容纳英文 Audio
    lv_obj_set_height(title_lbl, lv_font_get_line_height(fontpack_lv_font_ui()));
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    s_record_btn = lv_obj_create(row);
    lv_obj_remove_style_all(s_record_btn);
    lv_obj_set_size(s_record_btn, UiSx(152), UiSy(40)); // 容纳英文 Hold to talk
    lv_obj_set_style_bg_color(s_record_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_record_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_record_btn, kSettingsBorderW, 0);
    lv_obj_set_style_radius(s_record_btn, kSettingsOptionRadius, 0);
    lv_obj_clear_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, Lang::Strings::SETTINGS_TEST_AUDIO_HOLD);
    lv_obj_set_width(s_record_lbl, UiSx(140));
    lv_label_set_long_mode(s_record_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_record_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_record_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(s_record_lbl);
    lv_obj_clear_flag(s_record_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void SettingsTestAudio_OnLoad() {
    s_alive = true;
    s_state = State::Idle;
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
}
