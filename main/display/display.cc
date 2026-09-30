#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <font_awesome.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Display"

namespace {
std::atomic<bool> s_panel_busy{false};
}

void Display::SetPanelBusy(bool busy) {
    s_panel_busy.store(busy, std::memory_order_release);
}

bool Display::IsPanelBusy() {
    return s_panel_busy.load(std::memory_order_acquire);
}

bool Display::WaitPanelIdle(int timeout_ms) {
    if (!IsPanelBusy()) {
        return true;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    const int64_t deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    while (IsPanelBusy()) {
        if (timeout_ms == 0 || esp_timer_get_time() >= deadline_us) {
            return !IsPanelBusy();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

Display::Display() {
}

Display::~Display() {
}

void Display::SetIdleStatusMode(IdleStatusMode mode, const char* fixed_text) {
    idle_status_mode_ = mode;
    if (mode == IdleStatusMode::kFixedText && fixed_text != nullptr && fixed_text[0] != '\0') {
        std::snprintf(idle_status_fixed_text_, sizeof(idle_status_fixed_text_), "%s", fixed_text);
    } else {
        idle_status_fixed_text_[0] = '\0';
    }
}

bool Display::AllowsIdleStatusClock() const {
    return idle_status_mode_ == IdleStatusMode::kClock;
}

void Display::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {
}


void Display::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}
