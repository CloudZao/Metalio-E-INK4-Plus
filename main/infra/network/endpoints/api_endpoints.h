#pragma once

#include <string>
#include "cloudzao_endpoints.h"

inline const char* const kApiDefaultAppStoreUrl = cloudzao_app_store_url;
inline const char* const kApiHost = cloudzao_host;
inline const char* const kApiDefaultOtaUrl = cloudzao_ota_url;
inline const char* const kApiBookToolsUrl = cloudzao_book_tools_url;

inline const char* const kApiSinicloudToken = cloudzao_path_sinicloud_token;
inline const char* const kApiAsrTranscribe = cloudzao_path_asr_transcribe;
inline const char* const kApiAsrAudioRecords = cloudzao_path_asr_audio_records;
inline const char* const kApiChecklistItemsAll = cloudzao_path_checklist_items_all;
inline const char* const kApiChecklistItemPrefix = cloudzao_path_checklist_item_prefix;
inline const char* const kApiChecklistItemCompleteSuffix = cloudzao_path_checklist_item_complete_suffix;
inline const char* const kApiWeatherLatest = cloudzao_path_weather_latest;
inline const char* const kApiDeviceSettingsTts = cloudzao_path_device_settings_tts;
inline const char* const kApiPushResourcesNext = cloudzao_path_push_resources_next;
inline const char* const kApiPushResources = cloudzao_path_push_resources;
inline const char* const kApiLibrarySync = cloudzao_path_library_sync;
inline const char* const kApiDeviceLocationWifi = cloudzao_path_device_location_wifi;
inline const char* const kApiVisionUpload = cloudzao_path_vision_upload;

inline std::string ApiDeviceLogsUploadUrl(const std::string& mac_id) {
    if (!cloudzao_log_upload_host[0] || !cloudzao_path_device_logs[0] || mac_id.empty()) {
        return {};
    }
    return std::string(cloudzao_log_upload_host) + cloudzao_path_device_logs + "?mac_id=" + mac_id;
}

// Host is configured; blank open-source builds leave it empty.
inline bool ApiHasHost() {
    return kApiHost[0] != '\0';
}

// OTA URL is set when available.
inline bool ApiHasDefaultOtaUrl() {
    return kApiDefaultOtaUrl[0] != '\0';
}

// Return empty string when host or path is unavailable.
inline std::string ApiUrl(const char* path) {
    if (!ApiHasHost() || path == nullptr || path[0] == '\0') {
        return std::string();
    }
    return std::string(kApiHost) + path;
}

inline std::string ApiSinicloudTokenUrl() {
    return ApiUrl(kApiSinicloudToken);
}

inline std::string ApiAsrAudioRecordsUrl(const char* original_name) {
    std::string base = ApiUrl(kApiAsrAudioRecords);
    if (base.empty()) {
        return base;
    }
    if (original_name == nullptr || original_name[0] == '\0') {
        return base;
    }
    return base + original_name;
}

inline std::string ApiChecklistItemsAllUrl() {
    return ApiUrl(kApiChecklistItemsAll);
}

inline std::string ApiChecklistItemUrl(const char* item_id) {
    std::string base = ApiUrl(kApiChecklistItemPrefix);
    if (base.empty()) {
        return base;
    }
    if (item_id == nullptr || item_id[0] == '\0') {
        return base;
    }
    return base + item_id;
}

// POST /items/{id}/complete
inline std::string ApiChecklistItemCompleteUrl(const char* item_id) {
    std::string base = ApiChecklistItemUrl(item_id);
    if (base.empty() || kApiChecklistItemCompleteSuffix[0] == '\0') {
        return std::string();
    }
    return base + kApiChecklistItemCompleteSuffix;
}

inline std::string ApiWeatherLatestUrl() {
    return ApiUrl(kApiWeatherLatest);
}

inline std::string ApiDeviceSettingsTtsUrl() {
    return ApiUrl(kApiDeviceSettingsTts);
}

inline std::string ApiPushResourcesNextUrl() {
    return ApiUrl(kApiPushResourcesNext);
}

inline std::string ApiPushResourcesUrl() {
    return ApiUrl(kApiPushResources);
}

inline std::string ApiLibrarySyncUrl() {
    return ApiUrl(kApiLibrarySync);
}

inline std::string ApiDeviceLocationWifiUrl() {
    return ApiUrl(kApiDeviceLocationWifi);
}

inline std::string ApiVisionUploadUrl() {
    if (!cloudzao_vision_upload_host[0] || !kApiVisionUpload[0]) {
        return {};
    }
    return std::string(cloudzao_vision_upload_host) + kApiVisionUpload;
}

inline const char* const kApiWorkBuddyAuthorization = cloudzao_path_workbuddy_authorization;
inline const char* const kApiWorkBuddyAuthorizationSessions = cloudzao_path_workbuddy_authorization_sessions;
inline const char* const kApiWorkBuddyTextTurn = cloudzao_path_workbuddy_text_turn;
inline const char* const kApiWorkBuddyAsrStream = cloudzao_path_workbuddy_asr_stream;
inline const char* const kApiWorkBuddyLocalMessages = cloudzao_path_workbuddy_local_messages;
inline const char* const kApiWorkBuddyLocalStatus = cloudzao_path_workbuddy_local_status;
inline const char* const kApiWorkBuddyLocalReply = cloudzao_path_workbuddy_local_reply;
inline const char* const kApiWorkBuddyHost = cloudzao_workbuddy_host;

/** @brief 拼接 WorkBuddy 独立服务地址 */
inline std::string ApiWorkBuddyUrl(const char* path) {
    if (kApiWorkBuddyHost[0] == '\0' || path == nullptr || path[0] == '\0') {
        return {};
    }
    return std::string(kApiWorkBuddyHost) + path;
}
