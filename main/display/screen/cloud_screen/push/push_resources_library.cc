#include "cloud_screen/push/push_resources_library.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/push/push_resources_parse.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "board.h"
#include "power_policy.h"
#include "reader/book_library.h"
#include "reader/http_download_file.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <esp_log.h>
#include "assets/lang_config.h"

namespace reader {

bool DownloadFont(const CloudPushResource& item, std::string& err_out,
                  CloudDownloadProgressFn on_progress, void* progress_user, DownloadGate* gate) {
    err_out.clear();
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = Lang::Strings::CLOUD_CANCELLED;
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::CLOUD_NET_NOT_READY;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        err_out = Lang::Strings::CLOUD_NO_SD;
        return false;
    }
    if (!SdEnsureAppLayout()) {
        err_out = Lang::Strings::CLOUD_STORAGE_UNAVAIL;
        return false;
    }
    if (item.download_url.empty()) {
        err_out = Lang::Strings::CLOUD_NO_DOWNLOAD_URL;
        return false;
    }

    const std::string final_path = item.LocalPath();
    if (!IsAllowedFontLocalPath(final_path)) {
        err_out = Lang::Strings::CLOUD_PATH_INVALID;
        return false;
    }
    const std::string tmp_path = final_path + ".tmp";

    if (!DownloadHttpToFile(item.download_url.c_str(), tmp_path.c_str(),
                            item.sha256.empty() ? nullptr : item.sha256.c_str(), item.file_size,
                            kMaxFontDownloadBytes, on_progress, progress_user, err_out, gate)) {
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::CLOUD_CANCELLED;
        return false;
    }

    unlink(final_path.c_str());
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::CLOUD_SAVE_FAIL;
        return false;
    }
    ESP_LOGI(TAG, "saved cloud font -> %s", final_path.c_str());
    return true;
}

bool DownloadPushResource(const CloudPushResource& item, std::string& err_out,
                          CloudDownloadProgressFn on_progress, void* progress_user,
                          DownloadGate* gate, const uint8_t* cover_bytes, size_t cover_len) {
    err_out.clear();
    if (item.type == PushResourceType::kBook) {
        return DownloadCloudBook(ToBookTask(item), err_out, on_progress, progress_user, gate,
                                 cover_bytes, cover_len);
    }
    if (item.type == PushResourceType::kBadge) {
        return DownloadCloudWallpaper(ToWallpaperTask(item), err_out, on_progress, progress_user,
                                      gate);
    }
    if (item.type == PushResourceType::kFont) {
        return DownloadFont(item, err_out, on_progress, progress_user, gate);
    }
    err_out = Lang::Strings::CLOUD_UNKNOWN_TYPE;
    return false;
}

bool DeletePushResourceRemote(const CloudPushResource& item, std::string& err_out) {
    err_out.clear();
    if (item.task_id.empty()) {
        err_out = Lang::Strings::CLOUD_MISSING_TASK_ID;
        return false;
    }

    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::CLOUD_NET_NOT_READY;
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::CLOUD_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::CLOUD_CONN_FAIL;
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        err_out = Lang::Strings::CLOUD_JSON_CREATE_FAIL;
        return false;
    }
    cJSON_AddStringToObject(root, "taskId", item.task_id.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        err_out = Lang::Strings::CLOUD_JSON_SERIALIZE_FAIL;
        return false;
    }
    std::string body(printed);
    cJSON_free(printed);

    const std::string url = api::PushResourcesUrl();
    if (url.empty()) {
        ESP_LOGI(TAG, "skip push delete: cloud endpoints blank");
        return true;
    }
    http->SetTimeout(kHttpTimeoutMs);
    api::ApplyJsonHeaders(http);
    api::LogHttpRequest(TAG, "DELETE", url, body);
    http->SetContent(std::move(body));
    if (!http->Open("DELETE", url)) {
        err_out = Lang::Strings::CLOUD_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }

    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));
    if (status < 200 || status >= 300) {
        err_out = Lang::Strings::CLOUD_REQUEST_FAIL;
        return false;
    }

    if (!resp.empty()) {
        cJSON* resp_root = cJSON_Parse(resp.c_str());
        if (resp_root != nullptr) {
            const cJSON* code = cJSON_GetObjectItemCaseSensitive(resp_root, "code");
            if (cJSON_IsNumber(code) && code->valueint != 0) {
                const cJSON* msg = cJSON_GetObjectItemCaseSensitive(resp_root, "msg");
                if (cJSON_IsString(msg) && msg->valuestring != nullptr &&
                    msg->valuestring[0] != '\0') {
                    err_out = msg->valuestring;
                } else {
                    err_out = Lang::Strings::CLOUD_API_ERROR;
                }
                cJSON_Delete(resp_root);
                return false;
            }
            cJSON_Delete(resp_root);
        }
    }

    ESP_LOGI(TAG, "deleted push resource remote taskId=%s type=%s", item.task_id.c_str(),
             item.TypeLabel());
    return true;
}

}  // namespace reader
