#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <font_awesome.h>
#include "display.h"
#include "assets/lang_config.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "infra/storage/settings.h"
#include "ui/common/drawing/ui_theme.h"

#define TAG "Display"

static std::atomic<bool> s_panel_busy{false};

Display::Display() {
    UiThemeIsDark();
}

Display::~Display() {}

void Display::SetStatus(const char* status) {
    ESP_LOGD(TAG, "SetStatus bytes=%u", status != nullptr ? static_cast<unsigned>(std::strlen(status)) : 0);
}

void Display::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGD(TAG, "ShowNotification bytes=%u duration_ms=%d",
             notification != nullptr ? static_cast<unsigned>(std::strlen(notification)) : 0, duration_ms);
}

void Display::SetEmotion(const char* emotion) {
    ESP_LOGD(TAG, "SetEmotion provided=%d", emotion != nullptr);
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGD(TAG, "SetChatMessage user=%d assistant=%d bytes=%u", role != nullptr && std::strcmp(role, "user") == 0,
             role != nullptr && std::strcmp(role, "assistant") == 0,
             content != nullptr ? static_cast<unsigned>(std::strlen(content)) : 0);
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    if (!settings.SetString("theme", theme->name()) || !settings.Commit()) {
        ESP_LOGE(TAG, "operation=save_theme result=storage_error; RAM retained");
    }
}

void Display::UpdateStatusBar(bool update_all) {}

void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
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
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    while (IsPanelBusy()) {
        if (timeout_ms == 0 || esp_timer_get_time() >= deadline_us) {
            return !IsPanelBusy();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}
