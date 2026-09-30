#include "metalio_rtc.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"
#include "metalio_i2c1.h"
#include "pcf8563.h"
#include "power_i2c.h"

#include <atomic>
#include <driver/i2c.h>
#include <esp_log.h>

#define TAG "MetalioRtc"

#define EPDIY_I2C_PORT I2C_NUM_1

namespace {

std::atomic<bool> s_irq_pending{false};
bool s_p16_ok = false;
bool s_last_event = false; // 边沿：P16 低=有 INT

power_i2c_t MakeBus() {
    power_i2c_t bus = {
        .port = EPDIY_I2C_PORT,
        .lock = metalio_i2c1_lock,
        .unlock = metalio_i2c1_unlock,
    };
    return bus;
}

/** PCF8563 INT 低有效 → TCA P16；返回 true=有未清事件 */
bool RtcIntActive(void) {
    if (!epd_board_metalio_eink4_plus_ioexp_ok()) {
        return false;
    }
    return !epd_board_metalio_eink4_plus_ioexp_get_level(EPD_IO_RTC_INT);
}

void SampleRtcIntEdge(void) {
    if (!s_p16_ok) {
        return;
    }
    const bool active = RtcIntActive();
    if (active && !s_last_event) {
        s_irq_pending.store(true, std::memory_order_relaxed);
    }
    s_last_event = active;
}

} // namespace

void MetalioRtc_Init(void) {
    power_i2c_t bus = MakeBus();
    auto& rtc = Pcf8563::GetInstance();
    if (!rtc.Begin(&bus, PCF8563_I2C_ADDR)) {
        ESP_LOGW(TAG, "PCF8563 init skipped");
    } else if (rtc.ApplyRtcToSystem()) {
        ESP_LOGI(TAG, "boot time source: PCF8563");
    } else {
        ESP_LOGW(TAG, "PCF8563 online but time invalid (VL); wait NTP/set then SyncSystemToRtc");
    }

    s_p16_ok = epd_board_metalio_eink4_plus_ioexp_ok();
    if (s_p16_ok) {
        s_last_event = RtcIntActive();
        ESP_LOGI(TAG, "RTC_INT via TCA P%d ready (low=event, now=%s)", EPD_IO_RTC_INT,
                 s_last_event ? "active" : "idle");
    } else {
        ESP_LOGW(TAG, "RTC_INT P%d skipped (ioexp not ready)", EPD_IO_RTC_INT);
    }
}

bool MetalioRtc_Ok(void) {
    return Pcf8563::GetInstance().IsReady();
}

bool MetalioRtc_IrqPending(void) {
    SampleRtcIntEdge();
    return s_irq_pending.load(std::memory_order_relaxed);
}

bool MetalioRtc_ConsumeIrq(void) {
    SampleRtcIntEdge();
    const bool was = s_irq_pending.exchange(false, std::memory_order_relaxed);
    if (was && Pcf8563::GetInstance().IsReady()) {
        Pcf8563::GetInstance().ClearIrqFlags();
        s_last_event = RtcIntActive();
    }
    return was;
}

int MetalioRtc_GpioLevel(void) {
    // 对齐旧 GPIO4 语义：1=可能有未清 INT（P16 低）
    return RtcIntActive() ? 1 : 0;
}
