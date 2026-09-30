/**
 * @brief 尚未移植模块的薄 stub（WiFi 定位）
 * @note SD 已接 SdCardManager 实装，勿再 stub
 */
#include "device_wifi_location.h"

#include <esp_log.h>

#define TAG "ask_ai_stub"

namespace device_wifi_location {
void RequestBootReport() {
    ESP_LOGD(TAG, "device_wifi_location boot report stub");
}
} // namespace device_wifi_location
