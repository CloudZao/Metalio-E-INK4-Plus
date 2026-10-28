#include "metalio_accel.h"

#include "epd_board_metalio_eink4_plus.h"
#include "metalio_i2c1.h"
#include "power_i2c.h"
#include "sc7a20h.h"

#include <atomic>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MetalioAccel"
#define EPDIY_I2C_PORT I2C_NUM_1

namespace {

std::atomic<bool> s_reading_tracking{false};
TaskHandle_t s_accel_task = nullptr;
portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
int s_gravity_x = 0;
int s_gravity_y = 0;
int64_t s_sample_us = 0;

power_i2c_t MakeBus() {
    power_i2c_t bus = {
        .port = EPDIY_I2C_PORT,
        .lock = metalio_i2c1_lock,
        .unlock = metalio_i2c1_unlock,
        .device = metalio_i2c1_device_locked,
    };
    return bus;
}

void accel_irq_task(void* /*arg*/) {
    auto& accel = Sc7a20h::GetInstance();
    epd_board_metalio_eink4_plus_gpio2_irq_subscribe(xTaskGetCurrentTaskHandle());
    accel.ReadAndClearAoi1();

    while (true) {
        ulTaskNotifyTake(pdTRUE, s_reading_tracking.load() ? pdMS_TO_TICKS(200) : portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        // 轻睡前会关闭传感器；恢复阅读采样时在本任务重新配置。
        if (s_reading_tracking.load() && !accel.IsReady()) {
            power_i2c_t bus = MakeBus();
            if (!accel.Begin(&bus, Sc7a20h::kDefaultAddr) &&
                !accel.Begin(&bus, Sc7a20h::kAltAddr)) {
                continue;
            }
        }
        const bool motion = accel.ReadAndClearAoi1();
        if (!motion && !s_reading_tracking.load()) {
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
        if (s_reading_tracking.load()) {
            portENTER_CRITICAL(&s_sample_lock);
            s_gravity_x = ax;
            s_gravity_y = ay;
            s_sample_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_sample_lock);
        }
        if (motion) {
            ESP_LOGI(TAG, "INT1 x=%d y=%d z=%d mg", ax, ay, az);
        }
    }
}

} // namespace

void MetalioAccel_Init(void) {
    power_i2c_t bus = MakeBus();
    auto& accel = Sc7a20h::GetInstance();

    bool ok = accel.Begin(&bus, Sc7a20h::kDefaultAddr);
    if (!ok) {
        ok = accel.Begin(&bus, Sc7a20h::kAltAddr);
    }
    if (!ok) {
        ESP_LOGW(TAG, "SC7A20H not found @0x%02X/0x%02X", Sc7a20h::kDefaultAddr, Sc7a20h::kAltAddr);
        return;
    }

    if (xTaskCreate(accel_irq_task, "metalio_accel", 3072, nullptr, 5, &s_accel_task) != pdPASS) {
        ESP_LOGE(TAG, "accel task create failed");
    }
}

void MetalioAccel_ReleaseIntLine(void) {
    MetalioAccel_SetReadingTracking(false);
    if (Sc7a20h::GetInstance().IsReady()) {
        Sc7a20h::GetInstance().Deinit();
        ESP_LOGI(TAG, "INT released (was ready)");
        return;
    }
    power_i2c_t bus = MakeBus();
    auto& accel = Sc7a20h::GetInstance();
    bool ok = accel.SilenceSharedInt(&bus, Sc7a20h::kDefaultAddr);
    if (!ok) {
        ok = accel.SilenceSharedInt(&bus, Sc7a20h::kAltAddr);
    }
    ESP_LOGI(TAG, "INT release %s", ok ? "ok" : "skip (no chip)");
}

bool MetalioAccel_Ok(void) {
    return Sc7a20h::GetInstance().IsReady();
}

void MetalioAccel_SetReadingTracking(bool enabled) {
    if (s_reading_tracking.exchange(enabled) == enabled) {
        return;
    }
    portENTER_CRITICAL(&s_sample_lock);
    s_sample_us = 0;
    portEXIT_CRITICAL(&s_sample_lock);
    if (s_accel_task) {
        xTaskNotifyGive(s_accel_task);
    }
}

bool MetalioAccel_GetReadingGravity(int* x, int* y) {
    if (!x || !y || !s_reading_tracking.load()) {
        return false;
    }
    portENTER_CRITICAL(&s_sample_lock);
    *x = s_gravity_x;
    *y = s_gravity_y;
    const int64_t sampled = s_sample_us;
    portEXIT_CRITICAL(&s_sample_lock);
    return sampled != 0 && esp_timer_get_time() - sampled <= 500000;
}
