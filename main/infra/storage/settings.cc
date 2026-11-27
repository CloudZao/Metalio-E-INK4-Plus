#include <esp_log.h>
#include <nvs_flash.h>
#include "settings.h"

#define TAG "Settings"

static void LogReadFailure(const char* type, esp_err_t result) {
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "preference read failed type=%s code=0x%x", type, static_cast<unsigned>(result));
    }
}

Settings::Settings(const std::string& ns, bool read_write) : read_write_(read_write) {
    const esp_err_t result = nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    if (result != ESP_OK && (read_write_ || result != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGE(TAG, "preference open failed writable=%d code=0x%x", read_write_, static_cast<unsigned>(result));
    }
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_ && !commit_failed_) {
            Commit();
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    size_t length = 0;
    const esp_err_t length_result = nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length);
    if (length_result != ESP_OK) {
        LogReadFailure("string_length", length_result);
        return default_value;
    }
    std::string value;
    value.resize(length);
    const esp_err_t result = nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length);
    if (result != ESP_OK) {
        LogReadFailure("string", result);
        return default_value;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

bool Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_) {
        ESP_LOGE(TAG, "preference write rejected: read-only");
        write_failed_ = true;
        return false;
    }
    if (nvs_handle_ == 0) {
        write_failed_ = true;
        return false;
    }
    const esp_err_t result = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "preference write failed type=string code=0x%x", static_cast<unsigned>(result));
        write_failed_ = true;
        return false;
    }
    dirty_ = true;
    return true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    int32_t value;
    const esp_err_t result = nvs_get_i32(nvs_handle_, key.c_str(), &value);
    if (result != ESP_OK) {
        LogReadFailure("integer", result);
        return default_value;
    }
    return value;
}

bool Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_) {
        ESP_LOGE(TAG, "preference write rejected: read-only");
        write_failed_ = true;
        return false;
    }
    if (nvs_handle_ == 0) {
        write_failed_ = true;
        return false;
    }
    const esp_err_t result = nvs_set_i32(nvs_handle_, key.c_str(), value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "preference write failed type=integer code=0x%x", static_cast<unsigned>(result));
        write_failed_ = true;
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    uint8_t value;
    const esp_err_t result = nvs_get_u8(nvs_handle_, key.c_str(), &value);
    if (result != ESP_OK) {
        LogReadFailure("boolean", result);
        return default_value;
    }
    return value != 0;
}

bool Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_) {
        ESP_LOGE(TAG, "preference write rejected: read-only");
        write_failed_ = true;
        return false;
    }
    if (nvs_handle_ == 0) {
        write_failed_ = true;
        return false;
    }
    const esp_err_t result = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "preference write failed type=boolean code=0x%x", static_cast<unsigned>(result));
        write_failed_ = true;
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::EraseKey(const std::string& key) {
    if (!read_write_) {
        ESP_LOGE(TAG, "preference erase rejected: read-only");
        write_failed_ = true;
        return false;
    }
    if (nvs_handle_ == 0) {
        write_failed_ = true;
        return false;
    }
    const esp_err_t result = nvs_erase_key(nvs_handle_, key.c_str());
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "preference erase failed scope=key code=0x%x", static_cast<unsigned>(result));
        write_failed_ = true;
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::EraseAll() {
    if (!read_write_) {
        ESP_LOGE(TAG, "preference erase rejected: read-only");
        write_failed_ = true;
        return false;
    }
    if (nvs_handle_ == 0) {
        write_failed_ = true;
        return false;
    }
    const esp_err_t result = nvs_erase_all(nvs_handle_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "preference erase failed scope=namespace code=0x%x", static_cast<unsigned>(result));
        write_failed_ = true;
        return false;
    }
    dirty_ = true;
    return true;
}

bool Settings::Commit() {
    if (nvs_handle_ == 0 || !read_write_) {
        return false;
    }
    if (!dirty_) {
        return !commit_failed_ && !write_failed_;
    }
    const esp_err_t result = nvs_commit(nvs_handle_);
    commit_failed_ = result != ESP_OK;
    if (!commit_failed_) {
        dirty_ = false;
    } else {
        ESP_LOGE(TAG, "preference commit failed code=0x%x", static_cast<unsigned>(result));
    }
    return !commit_failed_ && !write_failed_;
}
