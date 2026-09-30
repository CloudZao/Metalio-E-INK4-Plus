#include "wifi_scan_lease.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace WifiScanLease {
namespace {

constexpr const char* kTag = "WifiScanLease";

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_held = false;
const char* s_owner = nullptr;

}  // namespace

bool TryAcquire(const char* owner_tag) {
    portENTER_CRITICAL(&s_mux);
    if (s_held) {
        const char* cur = s_owner;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(kTag, "acquire denied (held by %s), want %s", cur ? cur : "?",
                 owner_tag ? owner_tag : "?");
        return false;
    }
    s_held = true;
    s_owner = owner_tag;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(kTag, "acquired by %s", owner_tag ? owner_tag : "?");
    return true;
}

void Release() {
    portENTER_CRITICAL(&s_mux);
    const bool was = s_held;
    const char* cur = s_owner;
    s_held = false;
    s_owner = nullptr;
    portEXIT_CRITICAL(&s_mux);
    if (was) {
        ESP_LOGI(kTag, "released (was %s)", cur ? cur : "?");
    }
}

bool IsHeld() {
    portENTER_CRITICAL(&s_mux);
    const bool held = s_held;
    portEXIT_CRITICAL(&s_mux);
    return held;
}

}  // namespace WifiScanLease
