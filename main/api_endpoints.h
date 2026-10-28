#pragma once

#include "cloudzao_endpoints.h"

#include <string>

namespace api {

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

inline std::string DeviceLogsUploadUrl(const std::string& mac_id) {
    if (!cloudzao_log_upload_host[0] || !cloudzao_path_device_logs[0] || mac_id.empty()) return {};
    return std::string(cloudzao_log_upload_host) + cloudzao_path_device_logs + "?mac_id=" + mac_id;
}

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

inline const char* const kWorkBuddyAuthorization = cloudzao_path_workbuddy_authorization;
inline const char* const kWorkBuddyAuthorizationSessions = cloudzao_path_workbuddy_authorization_sessions;
inline const char* const kWorkBuddyTextTurn = cloudzao_path_workbuddy_text_turn;
inline const char* const kWorkBuddyAsrStream = cloudzao_path_workbuddy_asr_stream;
inline const char* const kWorkBuddyLocalMessages = cloudzao_path_workbuddy_local_messages;
inline const char* const kWorkBuddyLocalStatus = cloudzao_path_workbuddy_local_status;
inline const char* const kWorkBuddyLocalReply = cloudzao_path_workbuddy_local_reply;
inline const char* const kWorkBuddyHost = cloudzao_workbuddy_host;

/** @brief 拼接 WorkBuddy 独立服务地址 */
inline std::string WorkBuddyUrl(const char* path) {
    if (kWorkBuddyHost[0] == '\0' || path == nullptr || path[0] == '\0') return {};
    return std::string(kWorkBuddyHost) + path;
}

}  // namespace api
