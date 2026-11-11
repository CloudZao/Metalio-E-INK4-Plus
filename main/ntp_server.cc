#include "ntp_server.h"

#include "board.h"
#include "settings.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <sys/socket.h>

static constexpr const char* TAG = "NtpServer";
static constexpr int kUnknownOffset = 1000000;
static constexpr int kMaxOffsetSeconds = 14 * 60 * 60;
static std::atomic<bool> time_received{false};

static bool IsValidOffset(double offset) {
    return std::isfinite(offset) && offset >= -kMaxOffsetSeconds &&
           offset <= kMaxOffsetSeconds && std::trunc(offset) == offset;
}

NtpServer& NtpServer::GetInstance() {
    static NtpServer instance;
    return instance;
}

void NtpServer::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings("clock");
    const int offset = settings.GetInt("offset", kUnknownOffset);
    if (IsValidOffset(offset)) {
        SetTimezoneLocked(offset, settings.GetString("timezone"));
    }
}

bool NtpServer::SetTimezone(int offset, const std::string& timezone) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SetTimezoneLocked(offset, timezone);
}

bool NtpServer::SetTimezoneLocked(int offset, const std::string& timezone) {
    if (!IsValidOffset(offset) || timezone.size() > 96) {
        ESP_LOGE(TAG, "Timezone validation failed");
        return false;
    }
    // POSIX TZ 的符号与 UTC 偏移相反；当前版本仅使用接口提供的当前偏移。
    const int magnitude = std::abs(offset);
    char tz[32];
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d:%02d", offset < 0 ? '+' : '-',
             magnitude / 3600, magnitude / 60 % 60, magnitude % 60);
    if (setenv("TZ", tz, 1) != 0) {
        ESP_LOGE(TAG, "Timezone environment update failed");
        return false;
    }
    tzset();
    Settings settings("clock", true);
    if (settings.GetInt("offset", kUnknownOffset) != offset) settings.SetInt("offset", offset);
    if (settings.GetString("timezone") != timezone) settings.SetString("timezone", timezone);
    offset_seconds_.store(offset);
    timezone_ = timezone;
    timezone_valid_ = true;
    return true;
}

bool NtpServer::QueryTimezone(const std::string& ip, std::string& timezone, int& offset_seconds) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGE(TAG, "Timezone HTTP allocation failed");
        return false;
    }
    http->SetTimeout(8000);
    http->SetRequestDeadline(esp_timer_get_time() + 8000000);
    http->SetHeader("Accept", "application/json");
    const std::string url = "http://ipwho.is/" + ip + "?fields=success,timezone";
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Timezone connection failed: err=%d", http->GetLastError());
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    // 仅请求时区字段，并限制响应体，避免异常响应占用大量内存。
    char body[1025];
    size_t used = 0;
    int bytes = 0;
    if (status == 200) {
        while (used < sizeof(body) - 1 &&
               (bytes = http->Read(body + used, sizeof(body) - 1 - used)) > 0) {
            used += bytes;
        }
    }
    http->Close();
    if (status != 200 || bytes < 0 || used == sizeof(body) - 1) {
        ESP_LOGW(TAG, "Timezone response failed: status=%d read=%d bytes=%u",
                 status, bytes, static_cast<unsigned>(used));
        return false;
    }
    body[used] = '\0';
    cJSON* root = cJSON_Parse(body);
    const cJSON* zone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(zone, "id");
    const cJSON* offset = cJSON_GetObjectItemCaseSensitive(zone, "offset");
    const bool valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success")) &&
        cJSON_IsString(id) && id->valuestring[0] && cJSON_IsNumber(offset) &&
        IsValidOffset(offset->valuedouble) && strlen(id->valuestring) <= 96;
    if (valid) {
        timezone = id->valuestring;
        offset_seconds = static_cast<int>(offset->valuedouble);
    }
    cJSON_Delete(root);
    if (!valid) ESP_LOGW(TAG, "Timezone response invalid; retaining cached timezone");
    return valid;
}

bool NtpServer::Sync(const std::string& ip, Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result = {};
    unsigned char address[16];
    if (!ip.empty() && inet_pton(AF_INET, ip.c_str(), address) != 1 &&
        inet_pton(AF_INET6, ip.c_str(), address) != 1) {
        ESP_LOGE(TAG, "Timezone IP validation failed");
        return false;
    }
    std::string timezone = timezone_;
    int offset = offset_seconds_.load();
    const bool queried = QueryTimezone(ip, timezone, offset);
    if (!queried && (!timezone_valid_ || !ip.empty())) {
        ESP_LOGW(TAG, "NTP skipped: timezone unavailable");
        return false;
    }
    constexpr const char* servers[] = {"ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com"};
    for (unsigned i = 0; i < sizeof(servers) / sizeof(servers[0]); ++i) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(servers[i]);
        time_received.store(false);
        config.sync_cb = [](struct timeval*) { time_received.store(true); };
        ESP_LOGI(TAG, "Starting boot NTP sync: server=%u cached_timezone=%d", i, !queried);
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NTP init failed: err=%d", err);
            return false;
        }
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        esp_netif_sntp_deinit();
        // deinit 等待 TCP/IP 操作结束，也接受恰好在等待超时后到达的响应。
        if (!time_received.load()) {
            ESP_LOGW(TAG, "NTP sync failed: server=%u err=%d", i, err);
            continue;
        }
        if (!SetTimezoneLocked(offset, timezone)) return false;
        result.timezone = timezone;
        result.offset_seconds = offset;
        result.timezone_from_cache = !queried;
        result.utc_time = time(nullptr);
        if (!localtime_r(&result.utc_time, &result.local_time)) {
            ESP_LOGE(TAG, "NTP local time conversion failed");
            return false;
        }
        ESP_LOGI(TAG, "Boot NTP synchronized");
        return true;
    }
    return false;
}
