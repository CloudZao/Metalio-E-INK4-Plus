#include "ota_ignore.h"

#include "settings.h"

#include <esp_log.h>

namespace {

constexpr const char* TAG = "OtaIgnore";
constexpr const char* kNs = "ota";
constexpr const char* kKey = "skip_ver";

}  // namespace

namespace OtaIgnore {

std::string Get() {
    Settings settings(kNs, false);
    return settings.GetString(kKey, "");
}

void Set(const std::string& firmware_version) {
    if (firmware_version.empty()) {
        ESP_LOGW(TAG, "Set ignored: empty version");
        return;
    }
    Settings settings(kNs, true);
    settings.SetString(kKey, firmware_version);
    ESP_LOGI(TAG, "ignore version=%s", firmware_version.c_str());
}

void Clear() {
    Settings settings(kNs, true);
    settings.EraseKey(kKey);
    ESP_LOGI(TAG, "cleared");
}

bool IsIgnored(const std::string& firmware_version) {
    if (firmware_version.empty()) {
        return false;
    }
    const std::string skipped = Get();
    return !skipped.empty() && skipped == firmware_version;
}

}  // namespace OtaIgnore
