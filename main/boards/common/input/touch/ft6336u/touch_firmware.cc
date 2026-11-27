#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sdkconfig.h"
#include "touch_firmware.h"
#include "boards/common/bus/i2c/shared_i2c_bus.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"
#include "config.h"
#include "ft6336u_upgrade_protocol.h"

#if BOARD_TOUCH_UPGRADE_ON_BOOT
static constexpr const char* TAG = "touch_upgrade";

extern const uint8_t firmware_start[] asm("_binary_ft6336u_v04_app_bin_start");
extern const uint8_t firmware_end[] asm("_binary_ft6336u_v04_app_bin_end");

struct TouchFirmwareTransport {
    uint8_t reported_ids = 0;

    bool Write(const uint8_t* bytes, size_t size) {
        return shared_i2c_bus_transmit_locked(I2C_NUM_1, TOUCH_I2C_ADDR, bytes, size, 500) == ESP_OK;
    }

    bool Read(const uint8_t* command, size_t command_size, uint8_t* output, size_t size) {
        const esp_err_t err =
            shared_i2c_bus_transmit_receive_locked(I2C_NUM_1, TOUCH_I2C_ADDR, command, command_size, output, size, 500);
        const uint8_t bit = command[0] == 0xa3   ? 1
                            : command[0] == 0x9f ? 2
                            : command[0] == 0xa6 ? 4
                            : command[0] == 0x90 ? 8
                                                 : 0;
        if (bit && !(reported_ids & bit)) {
            reported_ids |= bit;
            ESP_LOGI(TAG, "ID reg=0x%02X result=%s byte0=0x%02X byte1=0x%02X", command[0],
                     esp_err_to_name(err), err == ESP_OK ? output[0] : 0, err == ESP_OK && size > 1 ? output[1] : 0);
        }
        return err == ESP_OK;
    }

    void Delay(int ms) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(ms)));
    }
};

#endif

bool TouchFirmware_UpgradeOnBoot() {
    #if BOARD_TOUCH_UPGRADE_ON_BOOT
    epd_board_adapter_tp_reset();  // 探测芯片前复位至工作模式。
    if (!shared_i2c_bus_lock(1000)) {
        ESP_LOGE(TAG, "I2C lock timeout; upgrade not started");
        return false;
    }
    TouchFirmwareTransport io;
    ESP_LOGI(TAG, "checking FT6336U for panel LT0470GF0430M, target V04");
    int attempts = 0;
    const auto result = Ft6336uUpgradeRun(io, firmware_start, firmware_end - firmware_start, &attempts);
    shared_i2c_bus_unlock();
    const bool ok = result == Ft6336uUpgradeResult::Current || result == Ft6336uUpgradeResult::Updated;
    if (ok) {
        ESP_LOGI(TAG, "touch firmware %s; attempts=%d",
                 result == Ft6336uUpgradeResult::Updated ? "updated and verified V04" : "already current; skipped",
                 attempts);
    } else {
        ESP_LOGE(TAG, "upgrade failed result=%d attempts=%d", static_cast<int>(result), attempts);
    }
    return ok;
    #else
    return true;
    #endif
}
