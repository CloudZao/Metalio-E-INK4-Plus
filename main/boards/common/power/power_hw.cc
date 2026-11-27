#include <esp_pm.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "power_hw.h"

static constexpr const char* TAG = "PowerHw";

static PowerHardwareCallbacks s_callbacks{};  // 启动时绑定，后台任务启动后只读

void power_hw_configure(const PowerHardwareCallbacks* callbacks) {
    if (callbacks) {
        s_callbacks = *callbacks;
    }
}
void power_hw_network_prepare_off() {
    if (s_callbacks.prepare_network_power_off) {
        s_callbacks.prepare_network_power_off();
    }
}
void power_hw_network_restored(bool recover) {
    if (s_callbacks.network_power_restored) {
        s_callbacks.network_power_restored(recover);
    }
}
void power_hw_network_resume() {
    if (s_callbacks.resume_network) {
        s_callbacks.resume_network();
    }
}
bool power_hw_sleep_cancelled() {
    return !s_callbacks.sleep_cancelled || s_callbacks.sleep_cancelled();
}

static StaticSemaphore_t s_cpu_freq_mutex_storage;
static SemaphoreHandle_t s_cpu_freq_mutex = xSemaphoreCreateMutexStatic(&s_cpu_freq_mutex_storage);
static unsigned s_cpu_freq_holds = 0;
static int s_cpu_freq_requested = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

static esp_err_t ApplyCpuFreq(int mhz) {
    #if CONFIG_PM_ENABLE
    if (mhz < 10) {
        mhz = 10;
    }
    esp_pm_config_t cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "cpu configured mhz=%d auto_ls=0", mhz);
    } else {
        ESP_LOGE(TAG, "cpu configuration failed mhz=%d result=%d", mhz, static_cast<int>(err));
    }
    return err;
    #else
    (void)mhz;
    ESP_LOGW(TAG, "cpu freq skipped (CONFIG_PM_ENABLE off)");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

esp_err_t power_hw_cpu_freq_set(int mhz) {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    const esp_err_t err = s_cpu_freq_holds ? ESP_OK : ApplyCpuFreq(mhz);
    if (err == ESP_OK) {
        s_cpu_freq_requested = mhz;
    }
    xSemaphoreGive(s_cpu_freq_mutex);
    return err;
}

PowerHwCpuFreqHold::PowerHwCpuFreqHold() {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    if (s_cpu_freq_holds == 0) {
        #if CONFIG_PM_ENABLE
        ESP_ERROR_CHECK(ApplyCpuFreq(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
        #endif
    }
    ++s_cpu_freq_holds;
    xSemaphoreGive(s_cpu_freq_mutex);
}

PowerHwCpuFreqHold::~PowerHwCpuFreqHold() {
    xSemaphoreTake(s_cpu_freq_mutex, portMAX_DELAY);
    if (--s_cpu_freq_holds == 0) {
        ApplyCpuFreq(s_cpu_freq_requested);
    }
    xSemaphoreGive(s_cpu_freq_mutex);
}
