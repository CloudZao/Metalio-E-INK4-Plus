#include "settings.h"

#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "Settings"

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    const esp_err_t result = nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    if (result != ESP_OK && (read_write_ || result != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGE(TAG, "preference open failed writable=%d code=0x%x", read_write_, static_cast<unsigned>(result));
    }
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            const esp_err_t result = nvs_commit(nvs_handle_);
            if (result != ESP_OK) ESP_LOGE(TAG, "preference commit failed code=0x%x", static_cast<unsigned>(result));
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }
    std::string value;
    value.resize(length);
    nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length);
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (read_write_) {
        if (nvs_handle_ == 0) return;
        const esp_err_t result = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
        if (result == ESP_OK) dirty_ = true;
        else ESP_LOGE(TAG, "preference write failed type=string code=0x%x", static_cast<unsigned>(result));
    } else {
        ESP_LOGE(TAG, "preference write rejected: read-only");
    }
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (read_write_) {
        if (nvs_handle_ == 0) return;
        const esp_err_t result = nvs_set_i32(nvs_handle_, key.c_str(), value);
        if (result == ESP_OK) dirty_ = true;
        else ESP_LOGE(TAG, "preference write failed type=integer code=0x%x", static_cast<unsigned>(result));
    } else {
        ESP_LOGE(TAG, "preference write rejected: read-only");
    }
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (read_write_) {
        nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        nvs_erase_key(nvs_handle_, key.c_str());
        dirty_ = true;
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        nvs_erase_all(nvs_handle_);
        dirty_ = true;
    }
}
