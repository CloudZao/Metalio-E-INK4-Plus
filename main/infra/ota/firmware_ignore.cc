#include <esp_log.h>
#include "firmware_ignore.h"
#include "infra/storage/settings.h"

static constexpr const char* TAG = "FirmwareIgnore";
static constexpr const char* kNs = "ota";
static constexpr const char* kKey = "skip_ver";

static std::string FirmwareIgnoreGet() {
    Settings settings(kNs, false);
    return settings.GetString(kKey, "");
}

void FirmwareIgnoreSet(const std::string& firmware_version) {
    if (firmware_version.empty()) {
        ESP_LOGW(TAG, "Set ignored: empty version");
        return;
    }
    Settings settings(kNs, true);
    if (!settings.SetString(kKey, firmware_version) || !settings.Commit()) {
        ESP_LOGE(TAG, "operation=ignore_version result=storage_error");
        return;
    }
    ESP_LOGI(TAG, "ignore version=%s", firmware_version.c_str());
}

void FirmwareIgnoreClear() {
    Settings settings(kNs, true);
    if (!settings.EraseKey(kKey) || !settings.Commit()) {
        ESP_LOGE(TAG, "operation=clear_ignored_version result=storage_error");
        return;
    }
    ESP_LOGI(TAG, "cleared");
}

bool FirmwareIgnoreIsIgnored(const std::string& firmware_version) {
    if (firmware_version.empty()) {
        return false;
    }
    const std::string skipped = FirmwareIgnoreGet();
    return !skipped.empty() && skipped == firmware_version;
}
