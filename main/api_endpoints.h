#pragma once

#include "cloudzao_endpoints.h"

#include <cstring>
#include <string>

#include <esp_log.h>

// Enable extra request/response body logging in debug builds.
#ifndef API_HTTP_DEBUG
#define API_HTTP_DEBUG 0
#endif

namespace api {

namespace detail {

inline const char* HttpBodyForLog(const std::string& body) {
    return body.empty() ? "(empty)" : body.c_str();
}

}  // namespace detail

// Log requests at debug level without noise in normal builds.
inline void LogHttpRequest(const char* tag, const char* method, const std::string& url,
                           const std::string& body = std::string(), const char* extra = nullptr) {
    ESP_LOGD(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGD(tag, "[API_HTTP] >>> %s", extra);
    }
#if API_HTTP_DEBUG
    ESP_LOGD(tag, "[API_HTTP] >>> body=%s", detail::HttpBodyForLog(body));
#else
    (void)body;
#endif
}

inline void LogHttpResponse(const char* tag, int status, const std::string& body) {
#if API_HTTP_DEBUG
    ESP_LOGD(tag, "[API_HTTP] <<< status=%d body=%s", status, detail::HttpBodyForLog(body));
#else
    ESP_LOGD(tag, "[API_HTTP] <<< status=%d", status);
    (void)body;
#endif
}

inline void LogHttpBinaryRequest(const char* tag, const char* method, const std::string& url,
                                 size_t body_size, const char* extra = nullptr) {
    ESP_LOGD(tag, "[API_HTTP] >>> %s %s body_bytes=%u", method, url.c_str(),
             static_cast<unsigned>(body_size));
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGD(tag, "[API_HTTP] >>> %s", extra);
    }
}

inline const char* const kHost = cloudzao_host;
inline const char* const kDefaultOtaUrl = cloudzao_ota_url;
inline const char* const kBookToolsUrl = cloudzao_book_tools_url;

inline const char* const kSinicloudToken = cloudzao_path_sinicloud_token;
inline const char* const kAsrTranscribe = cloudzao_path_asr_transcribe;
inline const char* const kAsrAudioRecords = cloudzao_path_asr_audio_records;
inline const char* const kChecklistItemsAll = cloudzao_path_checklist_items_all;
inline const char* const kChecklistItemPrefix = cloudzao_path_checklist_item_prefix;
inline const char* const kChecklistItemCompleteSuffix = cloudzao_path_checklist_item_complete_suffix;
inline const char* const kWeatherLatest = cloudzao_path_weather_latest;
inline const char* const kDeviceSettingsTts = cloudzao_path_device_settings_tts;
inline const char* const kPushResourcesNext = cloudzao_path_push_resources_next;
inline const char* const kPushResources = cloudzao_path_push_resources;
inline const char* const kLibrarySync = cloudzao_path_library_sync;
inline const char* const kDeviceLocationWifi = cloudzao_path_device_location_wifi;
inline const char* const kVisionUpload = cloudzao_path_vision_upload;

// Host is configured; blank open-source builds leave it empty.
inline bool HasHost() {
    return kHost[0] != '\0';
}

// OTA URL is set when available.
inline bool HasDefaultOtaUrl() {
    return kDefaultOtaUrl[0] != '\0';
}

// Return empty string when host or path is unavailable.
inline std::string Url(const char* path) {
    if (!HasHost() || path == nullptr || path[0] == '\0') {
        return std::string();
    }
    return std::string(kHost) + path;
}

inline std::string SinicloudTokenUrl() {
    return Url(kSinicloudToken);
}

inline std::string AsrAudioRecordsUrl(const char* original_name) {
    std::string base = Url(kAsrAudioRecords);
    if (base.empty()) {
        return base;
    }
    if (original_name == nullptr || original_name[0] == '\0') {
        return base;
    }
    return base + original_name;
}

inline std::string ChecklistItemsAllUrl() {
    return Url(kChecklistItemsAll);
}

inline std::string ChecklistItemUrl(const char* item_id) {
    std::string base = Url(kChecklistItemPrefix);
    if (base.empty()) {
        return base;
    }
    if (item_id == nullptr || item_id[0] == '\0') {
        return base;
    }
    return base + item_id;
}

// POST /items/{id}/complete
inline std::string ChecklistItemCompleteUrl(const char* item_id) {
    std::string base = ChecklistItemUrl(item_id);
    if (base.empty() || kChecklistItemCompleteSuffix[0] == '\0') {
        return std::string();
    }
    return base + kChecklistItemCompleteSuffix;
}

inline std::string WeatherLatestUrl() {
    return Url(kWeatherLatest);
}

inline std::string DeviceSettingsTtsUrl() {
    return Url(kDeviceSettingsTts);
}

inline std::string PushResourcesNextUrl() {
    return Url(kPushResourcesNext);
}

inline std::string PushResourcesUrl() {
    return Url(kPushResources);
}

inline std::string LibrarySyncUrl() {
    return Url(kLibrarySync);
}

inline std::string DeviceLocationWifiUrl() {
    return Url(kDeviceLocationWifi);
}

inline std::string VisionUploadUrl() {
    return Url(kVisionUpload);
}

// Redact URLs in log output when a host is configured.
inline std::string RedactClawUrlsForLog(const std::string& text) {
    if (!HasHost()) {
        return text;
    }
    std::string out = text;
    const size_t prefix_len = std::strlen(kHost);
    size_t pos = 0;
    while ((pos = out.find(kHost, pos)) != std::string::npos) {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + prefix_len);
        if (end == std::string::npos) {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;
    }
    return out;
}

}  // namespace api
