#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <esp_log.h>
#include <unistd.h>
#include "business/cloud/push_resources_library.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "infra/storage/sd_file_commit.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "infra/storage/sd_paths.h"
#include "business/cloud/push_resources_parse.h"
#include "business/cloud/push_resources_priv.h"
#include "business/cloud/wallpaper_cloud_library.h"
#include "business/reader/book_library.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "infra/network/http/http_download_file.h"
#include "sysmgmt/power/policy/power_policy.h"

bool DownloadFont(const CloudPushResource& item, std::string& err_out, CloudDownloadProgressFn on_progress,
                  void* progress_user, DownloadGate* gate, const char* save_path, bool overwrite) {
    err_out.clear();
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = LangString_CLOUD_NET_NOT_READY;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        err_out = LangString_CLOUD_NO_SD;
        return false;
    }
    if (!SdEnsureAppLayout()) {
        err_out = LangString_CLOUD_STORAGE_UNAVAIL;
        return false;
    }
    if (item.download_url.empty()) {
        err_out = LangString_CLOUD_NO_DOWNLOAD_URL;
        return false;
    }

    const std::string final_path = save_path != nullptr ? save_path : item.LocalPath();
    if (!IsAllowedFontLocalPath(final_path)) {
        err_out = LangString_CLOUD_PATH_INVALID;
        return false;
    }
    const std::string tmp_path = final_path + ".tmp";

    if (!DownloadHttpToFile(item.download_url.c_str(), tmp_path.c_str(),
                            item.sha256.empty() ? nullptr : item.sha256.c_str(), item.file_size, on_progress,
                            progress_user, err_out, gate)) {
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        unlink(tmp_path.c_str());
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }

    const auto commit = SdCommitFile(tmp_path, final_path, overwrite);
    if (commit != SdCommitResult::Ok) {
        err_out = commit == SdCommitResult::Conflict ? LangString_CLOUD_FILE_EXISTS : LangString_CLOUD_SAVE_FAIL;
        unlink(tmp_path.c_str());
        return false;
    }
    ESP_LOGI(kPushResourcesTag, "operation=save_font result=success");
    return true;
}

bool DownloadPushResource(const CloudPushResource& item, std::string& err_out, CloudDownloadProgressFn on_progress,
                          void* progress_user, DownloadGate* gate, const char* cover_path, const char* save_path,
                          bool overwrite) {
    err_out.clear();
    if (item.type == PushResourceType::kBook) {
        return DownloadCloudBook(ToBookTask(item), err_out, on_progress, progress_user, gate, cover_path, save_path,
                                 overwrite);
    }
    if (item.type == PushResourceType::kBadge) {
        return DownloadCloudWallpaper(ToWallpaperTask(item), err_out, on_progress, progress_user, gate, save_path,
                                      overwrite);
    }
    if (item.type == PushResourceType::kFont) {
        return DownloadFont(item, err_out, on_progress, progress_user, gate, save_path, overwrite);
    }
    err_out = LangString_CLOUD_UNKNOWN_TYPE;
    return false;
}

bool DeletePushResourceRemote(const CloudPushResource& item, std::string& err_out, DownloadGate* gate) try {
    err_out.clear();
    if (gate && gate->IsCancelled()) {
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }
    if (item.task_id.empty()) {
        err_out = LangString_CLOUD_MISSING_TASK_ID;
        return false;
    }

    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = LangString_CLOUD_NET_NOT_READY;
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = LangString_CLOUD_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = LangString_CLOUD_CONN_FAIL;
        return false;
    }

    DownloadGate::Binding binding(gate, http.get(), true);
    if (!binding) {
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        ESP_LOGE(kPushResourcesTag, "push delete failed stage=create_json");
        err_out = LangString_CLOUD_JSON_CREATE_FAIL;
        return false;
    }
    const bool valid = cJSON_AddStringToObject(root, "taskId", item.task_id.c_str()) != nullptr;
    std::unique_ptr<char, decltype(&cJSON_free)> printed(valid ? cJSON_PrintUnformatted(root) : nullptr, cJSON_free);
    cJSON_Delete(root);
    if (!printed) {
        ESP_LOGE(kPushResourcesTag, "push delete failed stage=serialize_json");
        err_out = LangString_CLOUD_JSON_SERIALIZE_FAIL;
        return false;
    }
    std::string body(printed.get());

    const std::string url = ApiPushResourcesUrl();
    if (url.empty()) {
        ESP_LOGI(kPushResourcesTag, "skip push delete: cloud endpoints blank");
        return true;
    }
    http->SetTimeout(kPushResourcesHttpTimeoutMs);
    ApiApplyJsonHeaders(http);
    http->SetContent(std::move(body));
    if (!http->Open("DELETE", url)) {
        err_out = LangString_CLOUD_REQUEST_FAIL;
        return false;
    }

    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        err_out = LangString_CLOUD_REQUEST_FAIL;
        return false;
    }

    if (!resp.empty()) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> resp_root(cJSON_Parse(resp.c_str()), cJSON_Delete);
        if (resp_root) {
            const cJSON* code = cJSON_GetObjectItemCaseSensitive(resp_root.get(), "code");
            if (cJSON_IsNumber(code) && code->valueint != 0) {
                const cJSON* msg = cJSON_GetObjectItemCaseSensitive(resp_root.get(), "msg");
                if (cJSON_IsString(msg) && msg->valuestring != nullptr && msg->valuestring[0] != '\0') {
                    err_out = msg->valuestring;
                } else {
                    err_out = LangString_CLOUD_API_ERROR;
                }
                return false;
            }
        }
    }

    ESP_LOGI(kPushResourcesTag, "operation=delete_remote_resource result=success type=%u", unsigned(item.type));
    return true;
} catch (const std::bad_alloc&) {
    err_out.clear();
    ESP_LOGE(kPushResourcesTag, "push delete failed stage=allocate");
    return false;
}
