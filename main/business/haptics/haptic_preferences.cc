#include <mutex>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sysmgmt/tasks/task_executor.h"
#include "haptic_preferences.h"
#include "boards/common/haptics/vibration.h"
#include "infra/storage/settings.h"

static const char* TAG = "Haptic";
static std::mutex s_mutex;
static bool s_ready = false;
static bool s_enabled = true;
static int s_intensity = 0;
static bool s_persist_running = false;

static int NormalizeIntensity(int intensity) {
    return intensity >= 0 && intensity <= 2 ? intensity : 0;
}

static void EnsureReady() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_ready) {
        return;
    }
    Settings settings("display");
    s_enabled = settings.GetBool("haptic", true);
    s_intensity = NormalizeIntensity(settings.GetInt("haptic_lvl", 0));
    s_ready = true;
    ESP_LOGI(TAG, "preferences loaded enabled=%d intensity=%d", s_enabled, s_intensity);
}

static void PersistTask(void*) {
    unsigned failures = 0;
    for (;;) {
        bool enabled;
        int intensity;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            enabled = s_enabled;
            intensity = s_intensity;
        }
        bool saved = false;
        try {
            Settings settings("display", true);
            const bool enabled_saved = settings.SetBool("haptic", enabled);
            const bool intensity_saved = settings.SetInt("haptic_lvl", intensity);
            saved = settings.Commit() && enabled_saved && intensity_saved;
        } catch (const std::bad_alloc&) {
            ESP_LOGE(TAG, "operation=save_preferences stage=allocate result=no_memory");
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (enabled != s_enabled || intensity != s_intensity) {
                failures = 0;
                continue;
            }
            if (saved || ++failures >= 3) {
                s_persist_running = false;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250 * failures));
    }
}

static void SchedulePersist() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_persist_running) {
        return;
    }
    s_persist_running = true;
    if (TaskExecutor::Schedule([] { PersistTask(nullptr); }, TaskDomain::Storage)) {
        return;
    }
    s_persist_running = false;
    ESP_LOGE(TAG, "operation=save_preferences stage=create_task result=no_memory; RAM retained");
}

bool HapticIsEnabled() {
    EnsureReady();
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_enabled;
}

int HapticGetIntensity() {
    EnsureReady();
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_intensity;
}

void HapticSetEnabled(bool enabled) {
    EnsureReady();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_enabled = enabled;
    }
    SchedulePersist();
}

void HapticSetIntensity(int intensity) {
    EnsureReady();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_intensity = NormalizeIntensity(intensity);
    }
    SchedulePersist();
}

void HapticPulseIfEnabled() {
    EnsureReady();
    int intensity;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_enabled) {
            return;
        }
        intensity = s_intensity;
    }
    Vibration_PulseFeedback(static_cast<uint8_t>(intensity));
}

uint32_t HapticPulseForIfEnabled(int duration_ms) {
    EnsureReady();
    int intensity;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_enabled) {
            return 0;
        }
        intensity = s_intensity;
    }
    if (intensity == 0) {
        return 0;
    }
    Vibration_SetIntensity(static_cast<uint8_t>(intensity + 1));
    return Vibration_PulseFor(duration_ms);
}
