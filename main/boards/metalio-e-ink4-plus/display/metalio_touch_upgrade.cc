#include "metalio_touch_upgrade.h"
#include "ft6336u_upgrade_protocol.h"
#include "metalio_i2c1.h"
#include "epd_board_metalio_eink4_plus.h"
#include "config.h"
#include "sdkconfig.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT
extern const uint8_t firmware_start[] asm("_binary_ft6336u_v04_app_bin_start");
extern const uint8_t firmware_end[] asm("_binary_ft6336u_v04_app_bin_end");
namespace {
struct Transport {
    uint8_t reported_ids = 0;
    bool Write(const uint8_t* bytes, size_t size) {
        return metalio_i2c1_transmit_locked(I2C_NUM_1, TOUCH_I2C_ADDR, bytes, size, 500) == ESP_OK;
    }
    bool Read(const uint8_t* command, size_t command_size, uint8_t* output, size_t size) {
        const esp_err_t err = metalio_i2c1_transmit_receive_locked(
            I2C_NUM_1, TOUCH_I2C_ADDR, command, command_size, output, size, 500);
        const uint8_t bit = command[0] == 0xa3 ? 1 : command[0] == 0x9f ? 2 :
                            command[0] == 0xa6 ? 4 : command[0] == 0x90 ? 8 : 0;
        if (bit && !(reported_ids & bit)) {
            reported_ids |= bit;
            ESP_LOGI("touch_upgrade", "ID reg=0x%02X result=%s byte0=0x%02X byte1=0x%02X",
                     command[0], esp_err_to_name(err), err == ESP_OK ? output[0] : 0,
                     err == ESP_OK && size > 1 ? output[1] : 0);
        }
        return err == ESP_OK;
    }
    void Delay(int ms) { vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(ms))); }
};
}
#endif

bool MetalioTouch_UpgradeOnBoot() {
    #ifdef CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT
    constexpr const char* tag = "touch_upgrade";
    epd_board_metalio_eink4_plus_tp_reset(); // 探测芯片前复位至工作模式。
    if (!metalio_i2c1_lock(1000)) {
        ESP_LOGE(tag, "I2C lock timeout; upgrade not started");
        return false;
    }
    Transport io;
    ESP_LOGI(tag, "checking FT6336U for panel LT0470GF0430M, target V04");
    int attempts = 0;
    const auto result = Ft6336uUpgrade::Run(io, firmware_start, firmware_end - firmware_start, &attempts);
    metalio_i2c1_unlock();
    const bool ok = result == Ft6336uUpgrade::Result::Current || result == Ft6336uUpgrade::Result::Updated;
    if (ok) ESP_LOGI(tag, "touch firmware %s; attempts=%d", result == Ft6336uUpgrade::Result::Updated ? "updated and verified V04" : "already current; skipped", attempts);
    else ESP_LOGE(tag, "upgrade failed result=%d attempts=%d", static_cast<int>(result), attempts);
    return ok;
    #else
    return true;
    #endif
}
