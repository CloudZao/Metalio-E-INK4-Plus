#include "ssid_manager.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <cJSON.h>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "SsidManager"

namespace {
constexpr size_t kMaxNetworks = 10;
constexpr size_t kMaxConfigBytes = 8192;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool ValidUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        unsigned count, value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 1;
            value = lead & 0x1f;
            minimum = 0x80;
        }
        else if (lead >= 0xe0 && lead <= 0xef) {
            count = 2;
            value = lead & 0xf;
            minimum = 0x800;
        }
        else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 3;
            value = lead & 7;
            minimum = 0x10000;
        }
        else return false;
        if (text.size() - i < count) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool ValidNetwork(const SsidItem& item) {
    if (item.ssid.empty() || item.ssid.size() > 32 ||
        item.ssid.find('\0') != std::string::npos ||
        item.password.find('\0') != std::string::npos ||
        !ValidUtf8(item.ssid) || !ValidUtf8(item.password)) return false;
    const size_t n = item.password.size();
    if (n == 0) return true;
    if (n == 64) {
        return std::all_of(item.password.begin(), item.password.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    }
    return n >= 8 && n <= 63;
}

bool Parse(const std::string& text, std::vector<SsidItem>& list) {
    auto invalid = [](const char* reason, int wifi = 0, const char* field = nullptr) {
        if (wifi > 0) {
            ESP_LOGE(TAG, "config invalid: wifi[%d] field=%s reason=%s", wifi, field ? field : "network", reason);
        } else if (field) {
            ESP_LOGE(TAG, "config invalid: root field=%s reason=%s", field, reason);
        } else {
            ESP_LOGE(TAG, "config invalid: %s", reason);
        }
        return false;
    };
    auto check_field = [&](int count, int wifi, const char* field) {
        return count == 1 || invalid(count == 0 ? "missing_field" : "duplicate_field", wifi, field);
    };
    if (text.empty() || text.size() > kMaxConfigBytes) return invalid("file_size");
    if (text.find('\0') != std::string::npos) return invalid("embedded_nul");
    if (!ValidUtf8(text)) return invalid("utf8_encoding");
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted && c == '\\') {
            ++i;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted) {
            if (static_cast<unsigned char>(c) < 0x20) return invalid("unescaped_control_character");
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t')
            return invalid("invalid_whitespace");
        // cJSON 接受部分非标准数字，先限定 JSON 数字语法。
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t end = i;
            if (text[end] == '-') ++end;
            if (end >= text.size()) return invalid("number_syntax");
            if (text[end] == '0') ++end;
            else {
                if (text[end] < '1' || text[end] > '9') return invalid("number_syntax");
                while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
            }
            if (end < text.size() && text[end] == '.') {
                const size_t first = ++end;
                while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
                if (end == first) return invalid("number_syntax");
            }
            if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
                ++end;
                if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
                const size_t first = end;
                while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
                if (end == first) return invalid("number_syntax");
            }
            if (end < text.size() && text[end] != ',' && text[end] != ']' && text[end] != '}' &&
                text[end] != ' ' && text[end] != '\n' && text[end] != '\r' && text[end] != '\t')
                return invalid("number_syntax");
            i = end - 1;
            continue;
        }
        if ((c == '{' || c == '[') && ++depth > 4) return invalid("nesting_limit");
        if (c == '}' || c == ']') {
            if (depth == 0) return invalid("json_syntax");
            --depth;
        }
    }
    // cJSON 的字符串接口不保留转义空字符后的长度，必须在解析前拒绝。
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') continue;
        if (text.compare(i, 6, "\\u0000") == 0) return invalid("escaped_nul");
        ++i;
    }
    const char* start = text.c_str();
    if (text.size() >= 3 && text.compare(0, 3, "\xef\xbb\xbf") == 0) start += 3;
    Json root(cJSON_ParseWithOpts(start, nullptr, true), cJSON_Delete);
    if (!root) return invalid("json_syntax");
    if (!cJSON_IsObject(root.get())) return invalid("root_type");
    int versions = 0, networks = 0;
    for (auto* field = root->child; field; field = field->next) {
        if (strcmp(field->string, "version") == 0) ++versions;
        else if (strcmp(field->string, "networks") == 0) ++networks;
        else return invalid("unknown_root_field");
    }
    const auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    const auto* entries = cJSON_GetObjectItemCaseSensitive(root.get(), "networks");
    const bool version_valid = check_field(versions, 0, "version");
    const bool networks_valid = check_field(networks, 0, "networks");
    if (!version_valid || !networks_valid) return false;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1) return invalid("unsupported_version", 0, "version");
    if (!cJSON_IsArray(entries)) return invalid("invalid_type", 0, "networks");
    if (cJSON_GetArraySize(entries) > kMaxNetworks) return invalid("too_many_networks", 0, "networks");
    std::vector<SsidItem> candidate;
    int wifi = 0;
    for (auto* entry = entries->child; entry; entry = entry->next) {
        ++wifi;
        if (!cJSON_IsObject(entry)) return invalid("invalid_type", wifi);
        int ssids = 0, passwords = 0;
        for (auto* field = entry->child; field; field = field->next) {
            if (strcmp(field->string, "ssid") == 0) ++ssids;
            else if (strcmp(field->string, "password") == 0) ++passwords;
            else return invalid("unknown_network_field", wifi);
        }
        const bool ssid_valid = check_field(ssids, wifi, "ssid");
        const bool password_valid = check_field(passwords, wifi, "password");
        if (!ssid_valid || !password_valid) return false;
        const auto* ssid = cJSON_GetObjectItemCaseSensitive(entry, "ssid");
        const auto* password = cJSON_GetObjectItemCaseSensitive(entry, "password");
        if (!cJSON_IsString(ssid)) return invalid("invalid_type", wifi, "ssid");
        if (!cJSON_IsString(password)) return invalid("invalid_type", wifi, "password");
        SsidItem item{ssid->valuestring, password->valuestring};
        if (item.ssid.empty() || item.ssid.size() > 32) return invalid("invalid_length", wifi, "ssid");
        if (!ValidNetwork(item)) return invalid(item.password.size() == 64 ? "invalid_hex_psk" : "invalid_length", wifi, "password");
        for (const auto& saved : candidate) {
            if (saved.ssid == item.ssid) return invalid("duplicate_ssid", wifi, "ssid");
        }
        candidate.push_back(std::move(item));
    }
    list = std::move(candidate);
    return true;
}

// 输入由 cJSON 生成，仅调整字符串外的空白，保留凭据中的转义内容。
std::string FormatJson(const char* compact) {
    std::string text;
    size_t depth = 0;
    auto newline = [&] { text += '\n'; text.append(depth * 2, ' '); };
    for (size_t i = 0; compact[i] != '\0'; ++i) {
        const char c = compact[i];
        if (c == '"') {
            text += c;
            while (compact[++i] != '\0') {
                text += compact[i];
                if (compact[i] == '\\') text += compact[++i];
                else if (compact[i] == '"') break;
            }
        } else if (c == '{' || c == '[') {
            text += c;
            ++depth;
            if (compact[i + 1] != '}' && compact[i + 1] != ']') newline();
        } else if (c == '}' || c == ']') {
            --depth;
            if (compact[i - 1] != '{' && compact[i - 1] != '[') newline();
            text += c;
        } else if (c == ',') {
            text += c;
            newline();
        } else if (c == ':') {
            text += ": ";
        } else {
            text += c;
        }
    }
    return text;
}

std::string Serialize(const std::vector<SsidItem>& list, bool formatted = false) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "version", 1)) return {};
    auto* entries = cJSON_AddArrayToObject(root.get(), "networks");
    if (!entries) return {};
    for (const auto& item : list) {
        auto* entry = cJSON_CreateObject();
        if (!entry) return {};
        if (!cJSON_AddItemToArray(entries, entry)) { cJSON_Delete(entry); return {}; }
        if (!cJSON_AddStringToObject(entry, "ssid", item.ssid.c_str()) ||
            !cJSON_AddStringToObject(entry, "password", item.password.c_str())) return {};
    }
    char* printed = cJSON_PrintUnformatted(root.get());
    if (!printed) return {};
    std::string text = formatted ? FormatJson(printed) : std::string(printed);
    cJSON_free(printed);
    return text;
}
}

SsidManager::SsidManager() { LoadFromNvs(); }

std::vector<SsidItem> SsidManager::GetSsidList() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssid_list_;
}

void SsidManager::LoadFromNvs() {
    nvs_handle_t handle;
    auto err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    size_t size = 0;
    err = nvs_get_blob(handle, "config", nullptr, &size);
    if (err == ESP_OK) {
        if (size < 2 || size > kMaxConfigBytes + 1) {
            ESP_LOGE(TAG, "NVS config invalid: record_size");
            nvs_close(handle);
            return;
        }
        std::string record(size, '\0');
        err = nvs_get_blob(handle, "config", &record[0], &size);
        nvs_close(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS config read failed: %s", esp_err_to_name(err));
            return;
        }
        if (record[0] != 0 && record[0] != 1) {
            ESP_LOGE(TAG, "NVS config invalid: pending_flag");
            return;
        }
        if (!Parse(record.substr(1), ssid_list_)) return;
        pending_ = record[0] == 1;
        nvs_ready_ = true;
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS config lookup failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    uint8_t initialized = 0;
    auto init_err = nvs_get_u8(handle, "initialized", &initialized);
    if (init_err != ESP_OK && init_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS initialization state read failed: %s", esp_err_to_name(init_err));
        nvs_close(handle);
        return;
    }
    bool valid = true;
    for (size_t i = 0; valid && i < kMaxNetworks; ++i) {
        const auto suffix = i ? std::to_string(i) : "";
        char ssid[33], password[65];
        size_t length = sizeof(ssid);
        err = nvs_get_str(handle, ("ssid" + suffix).c_str(), ssid, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK) {
            valid = false;
            break;
        }
        length = sizeof(password);
        err = nvs_get_str(handle, ("password" + suffix).c_str(), password, &length);
        if (err != ESP_OK || !ValidNetwork({ssid, password})) {
            valid = false;
            break;
        }
        ssid_list_.push_back({ssid, password});
    }
    nvs_close(handle);
    if (!valid) {
        ssid_list_.clear();
        ESP_LOGE(TAG, "NVS legacy read/validation failed: %s", esp_err_to_name(err));
        return;
    }
    if (init_err == ESP_ERR_NVS_NOT_FOUND && ssid_list_.empty()) {
        ssid_list_.push_back({"CloudZao-Test", "asdfghjkl"});
    }
    // 迁移本身不置待同步标记，以便已有 SD 配置仍按开机规则优先。
    nvs_ready_ = SaveToNvs(false);
}

bool SsidManager::SaveToNvs(bool pending) {
    const auto text = Serialize(ssid_list_);
    if (text.empty()) {
        ESP_LOGE(TAG, "NVS serialize failed");
        return false;
    }
    std::string record(1, pending ? 1 : 0);
    record += text;
    nvs_handle_t handle;
    auto err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    // 单个 blob 保存完整配置与待同步状态，避免逐项更新产生混合配置。
    err = nvs_set_blob(handle, "config", record.data(), record.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS blob write failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    // 新记录提交成功后再清理旧键；清理失败不回滚已提交的配置。
    esp_err_t cleanup = ESP_OK;
    bool removed = false;
    for (size_t i = 0; i < kMaxNetworks; ++i) {
        const auto suffix = i ? std::to_string(i) : "";
        for (const auto& key : {"ssid" + suffix, "password" + suffix}) {
            auto erased = nvs_erase_key(handle, key.c_str());
            if (erased == ESP_OK) removed = true;
            else if (erased != ESP_ERR_NVS_NOT_FOUND) cleanup = erased;
        }
    }
    auto erased = nvs_erase_key(handle, "initialized");
    if (erased == ESP_OK) removed = true;
    else if (erased != ESP_ERR_NVS_NOT_FOUND) cleanup = erased;
    auto committed = removed ? nvs_commit(handle) : ESP_OK;
    if (committed != ESP_OK) cleanup = committed;
    nvs_close(handle);
    if (cleanup != ESP_OK) ESP_LOGE(TAG, "NVS legacy cleanup failed: %s", esp_err_to_name(cleanup));
    pending_ = pending;
    nvs_ready_ = true;
    return true;
}

void SsidManager::SyncStorage() {
    if (!pending_ || !nvs_ready_ || !write_) return;
    auto text = Serialize(ssid_list_, true);
    if (text.empty()) {
        ESP_LOGE(TAG, "NVS->SD serialize failed");
        return;
    }
    text += '\n';
    if (!write_(text)) return;
    if (!SaveToNvs(false)) {
        ESP_LOGE(TAG, "NVS->SD pending acknowledgement failed; retry retained");
        return;
    }
    ESP_LOGI(TAG, "NVS->SD sync succeeded: %u networks", unsigned(ssid_list_.size()));
}

void SsidManager::InitializeStorage(ReadStorage read, WriteStorage write) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (read_ || write_) return;
    if (!read || !write) {
        ESP_LOGE(TAG, "storage initialization failed: missing callback");
        return;
    }
    read_ = read;
    write_ = write;
    if (pending_) {
        SyncStorage();
        return;
    }
    std::string text;
    const auto result = read_(text);
    if (result == ReadResult::Error) return;
    std::vector<SsidItem> imported;
    const bool repair = result == ReadResult::Invalid ||
        (result == ReadResult::Ok && !Parse(text, imported));
    if (result == ReadResult::Missing || repair) {
        if (repair) ESP_LOGW(TAG, "SD->NVS rejected; repairing SD from existing NVS");
        if (!nvs_ready_) {
            ESP_LOGE(TAG, "NVS->SD skipped: NVS unavailable");
            return;
        }
        if (SaveToNvs(true)) SyncStorage();
        return;
    }
    auto previous = std::move(ssid_list_);
    ssid_list_ = std::move(imported);
    if (!SaveToNvs(false)) {
        ssid_list_ = std::move(previous);
        ESP_LOGE(TAG, "SD->NVS sync failed: NVS persistence");
        return;
    }
    ESP_LOGI(TAG, "SD->NVS sync succeeded: %u networks", unsigned(ssid_list_.size()));
}

void SsidManager::RetryStorage() {
    std::lock_guard<std::mutex> lock(mutex_);
    SyncStorage();
}

void SsidManager::SaveChange(std::vector<SsidItem> previous) {
    if (!SaveToNvs(true)) {
        ssid_list_ = std::move(previous);
        ESP_LOGE(TAG, "WiFi change rejected: NVS persistence");
        return;
    }
    ESP_LOGI(TAG, "WiFi NVS saved: %u networks; SD sync pending", unsigned(ssid_list_.size()));
    SyncStorage();
}

void SsidManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto previous = std::move(ssid_list_);
    ssid_list_.clear();
    SaveChange(std::move(previous));
}

void SsidManager::AddSsid(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidNetwork({ssid, password})) {
        ESP_LOGE(TAG, "WiFi change rejected: invalid credential");
        return;
    }
    auto previous = ssid_list_;
    for (auto& item : ssid_list_) {
        if (item.ssid != ssid) continue;
        if (item.password == password) {
            SyncStorage();
            return;
        }
        item.password = password;
        SaveChange(std::move(previous));
        return;
    }
    if (ssid_list_.size() >= kMaxNetworks) ssid_list_.pop_back();
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    SaveChange(std::move(previous));
}

void SsidManager::RemoveSsid(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(ssid_list_.begin(), ssid_list_.end(), [&](const SsidItem& item) {
        return item.ssid == ssid;
    });
    if (found == ssid_list_.end()) return;
    auto previous = ssid_list_;
    ssid_list_.erase(found);
    SaveChange(std::move(previous));
}

void SsidManager::SetDefaultSsid(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(ssid_list_.begin(), ssid_list_.end(), [&](const SsidItem& item) {
        return item.ssid == ssid;
    });
    if (found == ssid_list_.end()) return;
    if (found == ssid_list_.begin()) {
        SyncStorage();
        return;
    }
    auto previous = ssid_list_;
    auto item = *found;
    ssid_list_.erase(found);
    ssid_list_.insert(ssid_list_.begin(), std::move(item));
    SaveChange(std::move(previous));
}
