#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <utility>
#include <sys/time.h>
#include <cJSON.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "sdkconfig.h"
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif
#include "device_config_client.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "infra/network/time/ntp_server.h"
#include "infra/platform/system_info.h"
#include "infra/storage/settings.h"

static constexpr const char* TAG = "DeviceConfig";
static std::string FirmwareConfigurationUrl() {
    return ApiHasDefaultOtaUrl() ? kApiDefaultOtaUrl : std::string();
}

DeviceConfigClient::DeviceConfigClient(std::function<bool()> cancelled, CloudConfiguration configuration)
    : cancelled_(std::move(cancelled)), configuration_(std::move(configuration)) {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // 设备序列号由 eFuse 提供。
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

std::unique_ptr<Http> DeviceConfigClient::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "operation=create_http result=no_network");
        return {};
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "operation=create_http result=no_memory");
        return {};
    }
    auto user_agent = SystemInfo::GetUserAgent();
    ApiApplyJsonHeaders(http);
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", LangCode);

    return http;
}

esp_err_t DeviceConfigClient::QueryFirmware() {
    return CheckResponse(false);
}

esp_err_t DeviceConfigClient::QueryChat() {
    return CheckResponse(true);
}

esp_err_t DeviceConfigClient::CheckResponse(bool chat_only) try {
    has_server_time_ = false;
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    has_mqtt_config_ = false;
    has_websocket_config_ = false;
    activation_code_.clear();
    activation_challenge_.clear();
    const int64_t begin = esp_timer_get_time();
    const int64_t deadline = begin + 30000000;
    esp_err_t result = ESP_FAIL;
    auto& board = Board::GetInstance();
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        if (board.IsWifiConfigMode() || (cancelled_ && cancelled_())) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        const int remaining = int((deadline - esp_timer_get_time()) / 1000);
        if (remaining <= 0) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (!board.IsNetworkReady() && !board.EnsureNetworkReady(std::min(remaining, 5000))) {
            if (board.GetNetworkErrorText(nullptr)) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
            result = ESP_ERR_TIMEOUT;
        } else {
            result = CheckResponseOnce(chat_only, std::min(deadline, esp_timer_get_time() + int64_t(8000000)));
        }
        const bool transient = result == ESP_FAIL || result == ESP_ERR_TIMEOUT || result == ESP_ERR_INVALID_RESPONSE ||
                               result == ESP_ERR_HTTP_CONNECT || result == ESP_ERR_HTTP_WRITE_DATA ||
                               result == ESP_ERR_HTTP_FETCH_HEADER || result == ESP_ERR_HTTP_EAGAIN ||
                               result == ESP_ERR_HTTP_CONNECTION_CLOSED || result == ESP_ERR_HTTP_READ_TIMEOUT ||
                               result == 408 || result == 429 || (result >= 500 && result <= 599);
        if (result == ESP_OK || !transient || attempt == 3) {
            break;
        }
        const int delay_ms = attempt * 500;
        if (deadline - esp_timer_get_time() <= int64_t(delay_ms) * 1000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=query_configuration result=no_memory");
    return ESP_ERR_NO_MEM;
}

esp_err_t DeviceConfigClient::CheckResponseOnce(bool chat_only, int64_t deadline_us) {
    chat_only_ = chat_only;
    firmware_version_.clear();
    firmware_url_.clear();
    auto& board = Board::GetInstance();

    std::string url = chat_only && configuration_.separate_chat_configuration ? configuration_.chat_configuration_url
                                                                              : FirmwareConfigurationUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "operation=query_configuration result=missing_endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    std::string data = board.GetSystemInfoJson();
    if (data.empty()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    int status_code = -1;
    auto http = SetupHttp();
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetRequestDeadline(deadline_us);
    const int remaining_ms = std::max(1, int((deadline_us - esp_timer_get_time()) / 1000));
    http->SetTimeout(remaining_ms);
    http->SetContent(std::move(data));
    if (esp_timer_get_time() >= deadline_us) {
        result = ESP_ERR_TIMEOUT;
    } else if (cancelled_ && cancelled_()) {
        result = ESP_ERR_INVALID_STATE;
    } else if (!http->Open("POST", url)) {
        result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_FAIL;
    } else {
        status_code = http->GetStatusCode();
        if (status_code <= 0) {
            result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_FAIL;
        } else {
            data = http->ReadAll();
            if (data.empty()) {
                result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (result != ESP_OK) {
        data.clear();
    }
    http->Close();
    if (result != ESP_OK) {
        return result;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    if (cancelled_ && cancelled_()) {
        return ESP_ERR_INVALID_STATE;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> response(cJSON_Parse(data.c_str()), cJSON_Delete);
    cJSON* root = response.get();
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "operation=query_configuration result=invalid_json");
        return ESP_ERR_INVALID_RESPONSE;
    }
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    // 固件查询不接纳对话凭据；对话查询不接纳固件元数据。
    if (chat_only) {
        cJSON* activation = cJSON_GetObjectItem(root, "activation");
        if (cJSON_IsObject(activation)) {
            cJSON* code = cJSON_GetObjectItem(activation, "code");
            if (cJSON_IsString(code)) {
                activation_code_ = code->valuestring;
                has_activation_code_ = true;
            }
            cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
            if (cJSON_IsString(challenge)) {
                activation_challenge_ = challenge->valuestring;
                has_activation_challenge_ = true;
            }
        }

        // 提供方切换传输方式后清除已经失效的凭据。
        if (configuration_.separate_chat_configuration) {
            if (!cJSON_IsObject(cJSON_GetObjectItem(root, "websocket"))) {
                Settings settings(configuration_.websocket_namespace.c_str(), true);
                if (!settings.EraseAll() || !settings.Commit()) {
                    return ESP_FAIL;
                }
            }
            if (!cJSON_IsObject(cJSON_GetObjectItem(root, "mqtt"))) {
                Settings settings(configuration_.mqtt_namespace.c_str(), true);
                if (!settings.EraseAll() || !settings.Commit()) {
                    return ESP_FAIL;
                }
            }
        }
        has_mqtt_config_ = false;
        cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
        if (cJSON_IsObject(mqtt)) {
            Settings settings(configuration_.mqtt_namespace.c_str(), true);
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, mqtt) {
                if (cJSON_IsString(item)) {
                    if (settings.GetString(item->string) != item->valuestring) {
                        settings.SetString(item->string, item->valuestring);
                    }
                } else if (cJSON_IsNumber(item)) {
                    if (settings.GetInt(item->string) != item->valueint) {
                        settings.SetInt(item->string, item->valueint);
                    }
                }
            }
            has_mqtt_config_ = settings.Commit();
            if (!has_mqtt_config_) {
                return ESP_FAIL;
            }
        }

        has_websocket_config_ = false;
        cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
        if (cJSON_IsObject(websocket)) {
            Settings settings(configuration_.websocket_namespace.c_str(), true);
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, websocket) {
                if (cJSON_IsString(item)) {
                    if (settings.GetString(item->string) != item->valuestring) {
                        settings.SetString(item->string, item->valuestring);
                    }
                } else if (cJSON_IsNumber(item)) {
                    if (settings.GetInt(item->string) != item->valueint) {
                        settings.SetInt(item->string, item->valueint);
                    }
                }
            }
            has_websocket_config_ = settings.Commit();
            if (!has_websocket_config_) {
                return ESP_FAIL;
            }
        } else {
            ESP_LOGI(TAG, "No websocket section found!");
        }

        has_server_time_ = false;
        cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
        if (cJSON_IsObject(server_time)) {
            cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
            cJSON* timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");

            if (cJSON_IsNumber(timestamp) && std::isfinite(timestamp->valuedouble) && timestamp->valuedouble >= 0 &&
                timestamp->valuedouble <= 253402300799999.0) {
                // 设置系统时间
                struct timeval tv{};
                const double ts = timestamp->valuedouble;

                // 云端时间戳保持 UTC，时区偏移单独用于当地时间显示。
                if (cJSON_IsNumber(timezone_offset)) {
                    const double minutes = timezone_offset->valuedouble;
                    if (std::isfinite(minutes) && minutes >= -840 && minutes <= 840 && std::trunc(minutes) == minutes) {
                        NtpServer::GetInstance().SetTimezone(static_cast<int>(minutes) * 60);
                    } else {
                        ESP_LOGW(TAG, "Server timezone offset invalid; retaining cached timezone");
                    }
                }

                tv.tv_sec = static_cast<time_t>(ts / 1000);
                tv.tv_usec = static_cast<decltype(tv.tv_usec)>(static_cast<long long>(ts) % 1000) * 1000;
                has_server_time_ = settimeofday(&tv, NULL) == 0;
                if (!has_server_time_) {
                    ESP_LOGE(TAG, "Server time apply failed: errno=%d", errno);
                }
            }
        } else {
            ESP_LOGW(TAG, "No server_time section found!");
        }
    }
    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (!chat_only && cJSON_IsObject(firmware)) {
        cJSON* version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON* url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

    } else if (!chat_only) {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    return ESP_OK;
}

std::string DeviceConfigClient::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32];  // SHA-256 输出为32字节

    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(),
                                       hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> payload(cJSON_CreateObject(), cJSON_Delete);
    if (!payload || !cJSON_AddStringToObject(payload.get(), "algorithm", "hmac-sha256") ||
        !cJSON_AddStringToObject(payload.get(), "serial_number", serial_number_.c_str()) ||
        !cJSON_AddStringToObject(payload.get(), "challenge", activation_challenge_.c_str()) ||
        !cJSON_AddStringToObject(payload.get(), "hmac", hmac_hex.c_str())) {
        return {};
    }
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(payload.get()), cJSON_free);
    return json ? std::string(json.get()) : std::string();
}

esp_err_t DeviceConfigClient::Activate() try {
    if ((cancelled_ && cancelled_())) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = CloudActivationUrl(chat_only_ && configuration_.separate_chat_configuration
                                             ? configuration_.chat_configuration_url
                                             : FirmwareConfigurationUrl());
    if (url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->SetTimeout(8000);
    http->SetRequestDeadline(esp_timer_get_time() + 8000000);
    std::string data = GetActivationPayload();
    if (data.empty()) {
        ESP_LOGE(TAG, "operation=activate stage=payload result=no_memory");
        return ESP_ERR_NO_MEM;
    }
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    http->ReadAll();
    if (status_code == 202) {
        http->Close();
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        http->Close();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    http->Close();
    return ESP_OK;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=activate result=no_memory");
    return ESP_ERR_NO_MEM;
}
