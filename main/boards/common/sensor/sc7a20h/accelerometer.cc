#include <atomic>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "accelerometer.h"
#include "infra/tasks/task_create.h"
#include "boards/common/bus/i2c/power_i2c.h"
#include "sc7a20h.h"

#define TAG "Accelerometer"
static power_i2c_t s_bus{};
static void (*s_subscribe_irq)() = nullptr;

static std::atomic<bool> s_reading_tracking{false};
static std::atomic<bool> s_reading_rapid{false};
static std::atomic<bool> s_app_tracking{false};
static TaskHandle_t s_accel_task = nullptr;
static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_gravity_x = 0;
static int s_gravity_y = 0;
static int s_gravity_z = 0;
static int64_t s_sample_us = 0;

static bool Tracking() {
    return s_reading_tracking.load() || s_app_tracking.load();
}

static void accel_irq_task(void* /*arg*/) {
    auto& accel = Sc7a20h::GetInstance();
    if (s_subscribe_irq) {
        s_subscribe_irq();
        accel.ReadAndClearAoi1();
    }

    while (true) {
        const bool rapid = s_app_tracking.load() || s_reading_rapid.load();
        ulTaskNotifyTake(pdTRUE, Tracking() ? pdMS_TO_TICKS(rapid ? 50 : 200) : portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        // 轻睡前会关闭传感器；恢复阅读采样时在本任务重新配置。
        if (Tracking() && !accel.IsReady()) {
            power_i2c_t bus = s_bus;
            if (!accel.Begin(&bus, Sc7a20h::kDefaultAddr) && !accel.Begin(&bus, Sc7a20h::kAltAddr)) {
                continue;
            }
        }
        const bool motion = s_subscribe_irq && accel.ReadAndClearAoi1();
        if (!motion && !Tracking()) {
            continue;
        }
        int ax = 0;
        int ay = 0;
        int az = 0;
        if (!accel.ReadAccelMg(ax, ay, az)) {
            portENTER_CRITICAL(&s_sample_lock);
            s_sample_us = 0;
            portEXIT_CRITICAL(&s_sample_lock);
            if (motion) {
                ESP_LOGW(TAG, "INT1 but ReadAccelMg failed");
            }
            continue;
        }
        if (Tracking()) {
            portENTER_CRITICAL(&s_sample_lock);
            s_gravity_x = ax;
            s_gravity_y = ay;
            s_gravity_z = az;
            s_sample_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_sample_lock);
        }
        if (motion) {
            ESP_LOGI(TAG, "INT1 x=%d y=%d z=%d mg", ax, ay, az);
        }
    }
}

void Accelerometer_Init(const power_i2c_t* configured_bus, void (*subscribe_irq)()) {
    s_bus = *configured_bus;
    s_subscribe_irq = subscribe_irq;
    power_i2c_t bus = s_bus;
    auto& accel = Sc7a20h::GetInstance();

    bool ok = accel.Begin(&bus, Sc7a20h::kDefaultAddr);
    if (!ok) {
        ok = accel.Begin(&bus, Sc7a20h::kAltAddr);
    }
    if (!ok) {
        ESP_LOGW(TAG, "SC7A20H not found @0x%02X/0x%02X", Sc7a20h::kDefaultAddr, Sc7a20h::kAltAddr);
        return;
    }

    if (FirmwareCreateTask(accel_irq_task, "accelerometer", 3072, nullptr, 5, &s_accel_task, tskNO_AFFINITY, 0) != pdPASS) {
        ESP_LOGE(TAG, "accel task create failed");
    }
}

void Accelerometer_ReleaseIntLine(void) {
    Accelerometer_SetReadingTracking(false, false);
    Accelerometer_SetAppTracking(false);
    if (Sc7a20h::GetInstance().IsReady()) {
        Sc7a20h::GetInstance().Deinit();
        ESP_LOGI(TAG, "INT released (was ready)");
        return;
    }
    power_i2c_t bus = s_bus;
    auto& accel = Sc7a20h::GetInstance();
    bool ok = accel.SilenceSharedInt(&bus, Sc7a20h::kDefaultAddr);
    if (!ok) {
        ok = accel.SilenceSharedInt(&bus, Sc7a20h::kAltAddr);
    }
    ESP_LOGI(TAG, "INT release %s", ok ? "ok" : "skip (no chip)");
}

bool Accelerometer_Ok(void) {
    return Sc7a20h::GetInstance().IsReady();
}

void Accelerometer_SetReadingTracking(bool enabled, bool rapid) {
    const bool rate_changed = s_reading_rapid.exchange(enabled && rapid) != (enabled && rapid);
    if (s_reading_tracking.exchange(enabled) == enabled && !rate_changed) {
        return;
    }
    portENTER_CRITICAL(&s_sample_lock);
    s_sample_us = 0;
    portEXIT_CRITICAL(&s_sample_lock);
    if (s_accel_task) {
        xTaskNotifyGive(s_accel_task);
    }
}

static bool GetGravity(int* x, int* y, int* z) {
    if (!x || !y || !Tracking()) {
        return false;
    }
    portENTER_CRITICAL(&s_sample_lock);
    *x = s_gravity_x;
    *y = s_gravity_y;
    if (z) {
        *z = s_gravity_z;
    }
    const int64_t sampled = s_sample_us;
    portEXIT_CRITICAL(&s_sample_lock);
    return sampled != 0 && esp_timer_get_time() - sampled <= 500000;
}

void Accelerometer_SetAppTracking(bool enabled) {
    if (s_app_tracking.exchange(enabled) == enabled) {
        return;
    }
    portENTER_CRITICAL(&s_sample_lock);
    s_sample_us = 0;
    portEXIT_CRITICAL(&s_sample_lock);
    if (s_accel_task) {
        xTaskNotifyGive(s_accel_task);
    }
}

bool Accelerometer_GetReadingGravity(int* x, int* y, int* z) {
    return s_reading_tracking.load() && GetGravity(x, y, z);
}

bool Accelerometer_GetAppGravity(int* x, int* y, int* z) {
    return s_app_tracking.load() && GetGravity(x, y, z);
}

AccelerometerOrientation Accelerometer_GetReadingOrientation() {
    int x = 0, y = 0;
    if (!Accelerometer_GetReadingGravity(&x, &y, nullptr)) {
        return AccelerometerOrientationUnknown;
    }
    constexpr int kGravityThresholdMg = 550;
    constexpr int kAxisDominanceMg = 150;
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    // 沿用当前板型的安装方向：X 对应竖屏，Y 正向对应左横屏。
    if (ay >= kGravityThresholdMg && ay >= ax + kAxisDominanceMg) {
        return y > 0 ? AccelerometerOrientationLeft : AccelerometerOrientationRight;
    }
    if (ax >= kGravityThresholdMg && ax >= ay + kAxisDominanceMg) {
        return AccelerometerOrientationPortrait;
    }
    return AccelerometerOrientationUnknown;
}
