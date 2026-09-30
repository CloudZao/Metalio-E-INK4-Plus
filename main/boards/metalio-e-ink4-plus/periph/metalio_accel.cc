#include "metalio_accel.h"

#include "epd_board_metalio_eink4_plus.h"
#include "metalio_i2c1.h"
#include "power_i2c.h"
#include "sc7a20h.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MetalioAccel"
#define EPDIY_I2C_PORT I2C_NUM_1

namespace {

power_i2c_t MakeBus() {
    power_i2c_t bus = {
        .port = EPDIY_I2C_PORT,
        .lock = metalio_i2c1_lock,
        .unlock = metalio_i2c1_unlock,
    };
    return bus;
}

void accel_irq_task(void* /*arg*/) {
    auto& accel = Sc7a20h::GetInstance();
    epd_board_metalio_eink4_plus_gpio2_irq_subscribe(xTaskGetCurrentTaskHandle());
    accel.ReadAndClearAoi1();

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        if (!accel.ReadAndClearAoi1()) {
            continue; // GPIO2 多半是 TCA 按键
        }
        int ax = 0;
        int ay = 0;
        int az = 0;
        if (!accel.ReadAccelMg(ax, ay, az)) {
            ESP_LOGW(TAG, "INT1 but ReadAccelMg failed");
            continue;
        }
        ESP_LOGI(TAG, "INT1 x=%d y=%d z=%d mg", ax, ay, az);
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

    xTaskCreate(accel_irq_task, "metalio_accel", 3072, nullptr, 5, nullptr);
}

void MetalioAccel_ReleaseIntLine(void) {
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
