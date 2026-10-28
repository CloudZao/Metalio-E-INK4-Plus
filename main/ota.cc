#include "sdkconfig.h"
#include "ota.h"
#include "display/epd_flash_guard.h"
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
#include "boards/common/sensor/pcf8563.h"
#include "boards/metalio-e-ink4-plus/bus/metalio_i2c1.h"
#endif
#include "api_endpoints.h"
#include "application.h"
#include "chat_cloud.h"
#include "api_http.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstdio>
#include <esp_heap_caps.h>
#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>

#define TAG "Ota"

namespace {
template <typename Operation>
auto WithOtaFlashAccess(Operation operation) {
    EpdFlashGuard guard;
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
    // Lock order: waveform/flash gate -> I2C1. The display uses the same order.
    // Block new touch/gauge/TPS commands only during the actual flash operation,
    // never during network waits or while posting LVGL callbacks.
    if (!metalio_i2c1_lock_for_flash(-1)) {
        return ESP_ERR_NO_MEM;
    }
    struct ReleaseI2c {
        ~ReleaseI2c() { metalio_i2c1_unlock(); }
    } release_i2c;
#endif
    return operation();
}
} // namespace


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
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

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    // Firmware updates always use our own endpoint, regardless of chat provider.
    return api::HasDefaultOtaUrl() ? api::kDefaultOtaUrl : std::string();
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    api::ApplyJsonHeaders(http);
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);

    return http;
}

esp_err_t Ota::CheckVersion() {
    return CheckResponse(false);
}

esp_err_t Ota::CheckChatConfiguration() {
    return CheckResponse(true);
}

esp_err_t Ota::CheckResponse(bool chat_only) {
    const int64_t begin = esp_timer_get_time();
    const int64_t deadline = begin + 30000000;
    esp_err_t result = ESP_FAIL;
    auto& board = Board::GetInstance();
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        if (board.IsWifiConfigMode() || Application::GetInstance().IsActivationPollingPaused()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        const int remaining = int((deadline - esp_timer_get_time()) / 1000);
        if (remaining <= 0) { result = ESP_ERR_TIMEOUT; break; }
        if (!board.IsNetworkReady() && !board.EnsureNetworkReady(std::min(remaining, 5000))) {
            if (board.GetNetworkErrorText(nullptr)) { result = ESP_ERR_INVALID_STATE; break; }
            result = ESP_ERR_TIMEOUT;
        } else {
            result = CheckResponseOnce(chat_only, std::min(deadline, esp_timer_get_time() + int64_t(8000000)));
        }
        const bool transient = result == ESP_FAIL || result == ESP_ERR_TIMEOUT || result == ESP_ERR_INVALID_RESPONSE ||
            result == ESP_ERR_HTTP_CONNECT || result == ESP_ERR_HTTP_WRITE_DATA || result == ESP_ERR_HTTP_FETCH_HEADER ||
            result == ESP_ERR_HTTP_EAGAIN || result == ESP_ERR_HTTP_CONNECTION_CLOSED || result == ESP_ERR_HTTP_READ_TIMEOUT ||
            result == 408 || result == 429 || (result >= 500 && result <= 599);
        if (result == ESP_OK || !transient || attempt == 3) break;
        const int delay_ms = attempt * 500;
        if (deadline - esp_timer_get_time() <= int64_t(delay_ms) * 1000) break;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return result;
}

esp_err_t Ota::CheckResponseOnce(bool chat_only, int64_t deadline_us) {

    chat_only_ = chat_only;
    has_new_version_ = false;
    firmware_version_.clear();
    firmware_url_.clear();
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;

    std::string url = chat_only && chat_cloud::UsesXiaozhiProtocol()
        ? chat_cloud::ChatOtaUrl() : GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    std::string data = board.GetSystemInfoJson();
    std::string method = data.empty() ? "GET" : "POST";
    esp_err_t result = ESP_OK;
    int status_code = -1;
    auto http = SetupHttp();
    if (!http) return ESP_ERR_NO_MEM;
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetRequestDeadline(deadline_us);
    const int remaining_ms = std::max(1, int((deadline_us - esp_timer_get_time()) / 1000));
    http->SetTimeout(remaining_ms);
    http->SetContent(std::move(data));
    if (esp_timer_get_time() >= deadline_us) {
        result = ESP_ERR_TIMEOUT;
    } else if (!http->Open(method, url)) {
        result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_FAIL;
    } else {
        status_code = http->GetStatusCode();
        if (status_code <= 0) {
            result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_FAIL;
        } else {
            data = http->ReadAll();
            if (data.empty()) result = esp_timer_get_time() >= deadline_us ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (result != ESP_OK) data.clear();
    http->Close();
    if (result != ESP_OK) return result;
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    // Firmware checks must never supply AI activation or transport configuration.
    if (chat_only) {
        cJSON *activation = cJSON_GetObjectItem(root, "activation");
        if (cJSON_IsObject(activation)) {
            cJSON* message = cJSON_GetObjectItem(activation, "message");
            if (cJSON_IsString(message)) {
                activation_message_ = message->valuestring;
            }
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
            cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
            if (cJSON_IsNumber(timeout_ms)) {
                activation_timeout_ms_ = timeout_ms->valueint;
            }
        }

        // Drop obsolete Xiaozhi transport credentials when the server switches transport.
        if (chat_cloud::UsesXiaozhiProtocol()) {
            if (!cJSON_IsObject(cJSON_GetObjectItem(root, "websocket"))) {
                Settings settings(chat_cloud::WebsocketNamespace(), true);
                settings.EraseAll();
            }
            if (!cJSON_IsObject(cJSON_GetObjectItem(root, "mqtt"))) {
                Settings settings(chat_cloud::MqttNamespace(), true);
                settings.EraseAll();
            }
        }
        has_mqtt_config_ = false;
        cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
        if (cJSON_IsObject(mqtt)) {
            Settings settings(chat_cloud::MqttNamespace(), true);
            cJSON *item = NULL;
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
            has_mqtt_config_ = true;
        }

        has_websocket_config_ = false;
        cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
        if (cJSON_IsObject(websocket)) {
            Settings settings(chat_cloud::WebsocketNamespace(), true);
            cJSON *item = NULL;
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
            has_websocket_config_ = true;
        } else {
            ESP_LOGI(TAG, "No websocket section found!");
        }

        has_server_time_ = false;
        cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
        if (cJSON_IsObject(server_time)) {
            cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
            cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
            if (cJSON_IsNumber(timestamp)) {
                // 设置系统时间
                struct timeval tv;
                double ts = timestamp->valuedouble;
            
                // 如果有时区偏移，计算本地时间
                if (cJSON_IsNumber(timezone_offset)) {
                    ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
                }
            
                tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
                tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
                has_server_time_ = settimeofday(&tv, NULL) == 0;
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
                if (has_server_time_ && !Pcf8563::GetInstance().SyncSystemToRtc()) {
                    ESP_LOGW(TAG, "Server time applied but RTC writeback failed");
                }
#endif
            }
        } else {
            ESP_LOGW(TAG, "No server_time section found!");
        }

    }
    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    // Only our firmware check can supply an upgrade version and download URL.
    if (!chat_only && cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            }
            // 忽略服务端 firmware.force：仅按版本比较决定是否提示升级
        }
    } else if (!chat_only) {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        WithOtaFlashAccess([&]() { return esp_ota_mark_app_valid_cancel_rollback(); });
    }
}

bool Ota::Upgrade(const std::string& firmware_url) {
    esp_ota_handle_t update_handle = 0;
    struct AbortIncompleteOta {
        esp_ota_handle_t& handle;
        ~AbortIncompleteOta() {
            if (handle != 0) {
                WithOtaFlashAccess([&]() { return esp_ota_abort(handle); });
            }
        }
    } abort_incomplete{update_handle};
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", status);
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > update_partition->size) {
        ESP_LOGE(TAG, "Invalid firmware size: %u (partition %lu)",
                 static_cast<unsigned>(content_length), update_partition->size);
        return false;
    }

    constexpr size_t kBufferSize = 8192;
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        &heap_caps_free);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        return false;
    }
    size_t buffered = 0;
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer.get() + buffered, kBufferSize - buffered);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false;
        }

        buffered += static_cast<size_t>(ret);
        if (ret == 0 && image_header_checked && total_read == content_length && buffered > 0) {
            const auto tail_err = WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, buffer.get(), buffered); });
            if (tail_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA tail: %s", esp_err_to_name(tail_err));
                return false;
            }
            buffered = 0;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (total_read > content_length) {
            ESP_LOGE(TAG, "Firmware exceeds advertised length");
            return false;
        }
        if (!image_header_checked) {
            constexpr size_t kHeaderNeed = sizeof(esp_image_header_t) +
                sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
            if (buffered < kHeaderNeed) {
                continue; // A TCP read may return less than the complete image header.
            }
            esp_image_header_t header{};
            memcpy(&header, buffer.get(), sizeof(header));
            esp_app_desc_t new_app_info{};
            memcpy(&new_app_info, buffer.get() + sizeof(esp_image_header_t) +
                   sizeof(esp_image_segment_header_t), sizeof(new_app_info));
            if (header.magic != ESP_IMAGE_HEADER_MAGIC || new_app_info.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                ESP_LOGE(TAG, "Invalid firmware image header");
                return false;
            }
            // SDK 按固件长度预擦除：对齐区域使用 64 KiB 块，尾部按扇区处理。
            const auto begin_err = WithOtaFlashAccess([&]() {
                return esp_ota_begin(update_partition, content_length, &update_handle);
            });
            if (begin_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(begin_err));
                return false;
            }
            image_header_checked = true;
        }
        // 复用读取缓冲，攒满后写入；尾部在正常 EOF 时写入。
        if (buffered < kBufferSize) continue;
        auto err = WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, buffer.get(), buffered); });
        buffered = 0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            return false;
        }
    }
    http->Close();

    if (!image_header_checked || total_read != content_length) {
        ESP_LOGE(TAG, "Incomplete firmware: %u/%u", static_cast<unsigned>(total_read),
                 static_cast<unsigned>(content_length));
        return false;
    }
    esp_err_t err = WithOtaFlashAccess([&]() { return esp_ota_end(update_handle); });
    update_handle = 0; // esp_ota_end consumes the handle on both success and failure.
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = WithOtaFlashAccess([&]() { return esp_ota_set_boot_partition(update_partition); });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return Upgrade(firmware_url_);
}

bool Ota::StartUpgradeFromUrl(const std::string& url, std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return Upgrade(url);
}

bool Ota::StartUpgradeFromFile(const std::string& path,
                               std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return UpgradeFromFile(path);
}

bool Ota::PeekFirmwareVersionFromFile(const std::string& path, std::string& out_version) {
    out_version.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }

    constexpr size_t kNeed = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                             sizeof(esp_app_desc_t);
    char buf[kNeed];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < kNeed) {
        return false;
    }

    esp_app_desc_t desc{};
    memcpy(&desc, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(esp_app_desc_t));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD || desc.version[0] == '\0') {
        return false;
    }
    out_version.assign(desc.version);
    return true;
}

bool Ota::UpgradeFromFile(const std::string& path) {
    ESP_LOGI(TAG, "Upgrading firmware from file %s", path.c_str());

    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGE(TAG, "SD firmware missing or empty: %s", path.c_str());
        return false;
    }
    const size_t content_length = static_cast<size_t>(st.st_size);

    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        return false;
    }

    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        ESP_LOGE(TAG, "Failed to get update partition");
        fclose(f);
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label,
             update_partition->address);

    constexpr size_t kHeaderNeed = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                                   sizeof(esp_app_desc_t);
    bool image_header_checked = false;
    std::string image_header;
    image_header.reserve(kHeaderNeed);

    char buffer[512];
    size_t total_read = 0;
    size_t recent_read = 0;
    auto last_calc_time = esp_timer_get_time();

    while (total_read < content_length) {
        const size_t to_read = std::min(sizeof(buffer), content_length - total_read);
        const size_t n = fread(buffer, 1, to_read, f);
        if (n == 0) {
            ESP_LOGE(TAG, "Unexpected EOF reading firmware (%u/%u)",
                     static_cast<unsigned>(total_read), static_cast<unsigned>(content_length));
            if (update_handle != 0) {
                WithOtaFlashAccess([&]() { return esp_ota_abort(update_handle); });
            }
            fclose(f);
            return false;
        }

        recent_read += n;
        total_read += n;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || total_read >= content_length) {
            const size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", static_cast<unsigned>(progress),
                     static_cast<unsigned>(total_read), static_cast<unsigned>(content_length),
                     static_cast<unsigned>(recent_read));
            if (upgrade_callback_) {
                upgrade_callback_(static_cast<int>(progress), recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, n);
            if (image_header.size() < kHeaderNeed) {
                continue;
            }

            esp_app_desc_t new_app_info{};
            memcpy(&new_app_info,
                   image_header.data() + sizeof(esp_image_header_t) +
                       sizeof(esp_image_segment_header_t),
                   sizeof(esp_app_desc_t));

            if (WithOtaFlashAccess([&]() { return esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle); }) !=
                ESP_OK) {
                ESP_LOGE(TAG, "Failed to begin OTA");
                WithOtaFlashAccess([&]() { return esp_ota_abort(update_handle); });
                fclose(f);
                return false;
            }

            auto err = WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, image_header.data(), image_header.size()); });
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA header: %s", esp_err_to_name(err));
                WithOtaFlashAccess([&]() { return esp_ota_abort(update_handle); });
                fclose(f);
                return false;
            }
            image_header_checked = true;
            std::string().swap(image_header);
            continue;
        }

        auto err = WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, buffer, n); });
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            WithOtaFlashAccess([&]() { return esp_ota_abort(update_handle); });
            fclose(f);
            return false;
        }
    }
    fclose(f);

    if (!image_header_checked) {
        ESP_LOGE(TAG, "Firmware file too small / invalid header");
        return false;
    }

    esp_err_t err = WithOtaFlashAccess([&]() { return esp_ota_end(update_handle); });
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = WithOtaFlashAccess([&]() { return esp_ota_set_boot_partition(update_partition); });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade from file successful");
    return true;
}

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
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

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    return json;
}

esp_err_t Ota::Activate() {
    if (Application::GetInstance().IsActivationPollingPaused()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = chat_cloud::ActivationUrl(chat_only_ && chat_cloud::UsesXiaozhiProtocol()
        ? chat_cloud::ChatOtaUrl() : GetCheckVersionUrl());
    if (url.empty()) { return ESP_ERR_INVALID_ARG; }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    const auto response_body = http->ReadAll();
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
}
