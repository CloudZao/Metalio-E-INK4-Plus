#include "cloud_screen/push/push_resources_parse.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/push/push_resources_library.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "board.h"
#include "power_policy.h"
#include "reader/book_library.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cJSON.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"

namespace reader {

bool ParseUint64Field(const cJSON* node, uint64_t* out) {
    if (out == nullptr || node == nullptr) {
        return false;
    }
    if (cJSON_IsNumber(node)) {
        if (node->valuedouble < 0) {
            return false;
        }
        *out = static_cast<uint64_t>(node->valuedouble);
        return true;
    }
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(node->valuestring, &end, 10);
        if (end == node->valuestring) {
            return false;
        }
        *out = static_cast<uint64_t>(v);
        return true;
    }
    return false;
}

PushResourceType ParseResourceType(const char* s) {
    if (s == nullptr) {
        return PushResourceType::kUnknown;
    }
    if (std::strcmp(s, "BOOK") == 0) {
        return PushResourceType::kBook;
    }
    if (std::strcmp(s, "BADGE") == 0) {
        return PushResourceType::kBadge;
    }
    if (std::strcmp(s, "FONT") == 0) {
        return PushResourceType::kFont;
    }
    return PushResourceType::kUnknown;
}

std::string SanitizeBasename(const std::string& raw) {
    std::string file = raw;
    const size_t slash = file.find_last_of("/\\");
    if (slash != std::string::npos) {
        file = file.substr(slash + 1);
    }
    if (file.empty() || file == "." || file == "..") {
        return {};
    }
    return file;
}

// TTF/OTF 落 fonts_ttf；其余（含 .ef）落 fonts。
bool IsTtfOrOtfBasename(const std::string& file) {
    const size_t n = file.size();
    if (n < 5) {
        return false;
    }
    const char* ext = file.c_str() + (n - 4);
    return strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0;
}

bool IsAllowedFontLocalPath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const std::string fonts_prefix = std::string(SD_PATH_FONTS) + "/";
    const std::string ttf_prefix = std::string(SD_PATH_FONT_SRC) + "/";
    return path.rfind(fonts_prefix, 0) == 0 || path.rfind(ttf_prefix, 0) == 0;
}

std::string ReadHttpBodyProgress(Http* http, size_t max_bytes, std::string& err_out) {
    err_out.clear();
    if (http == nullptr) {
        err_out = Lang::Strings::CLOUD_CONN_FAIL;
        return {};
    }
    const size_t expect = http->GetBodyLength();
    if (expect > max_bytes) {
        err_out = Lang::Strings::CLOUD_LIST_TOO_LARGE;
        return {};
    }

    std::string body;
    if (expect > 0) {
        body.reserve(expect);
    }
    char buf[1024];
    const int64_t t0 = esp_timer_get_time();
    int64_t last_log = t0;
    while (true) {
        const int n = http->Read(buf, sizeof(buf));
        if (n < 0) {
            err_out = Lang::Strings::CLOUD_READ_TIMEOUT;
            return {};
        }
        if (n == 0) {
            break;
        }
        if (body.size() + static_cast<size_t>(n) > max_bytes) {
            err_out = Lang::Strings::CLOUD_LIST_TOO_LARGE;
            return {};
        }
        body.append(buf, static_cast<size_t>(n));
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 2000000) {
            ESP_LOGI(TAG, "Read progress %u/%u bytes %dms", static_cast<unsigned>(body.size()),
                     static_cast<unsigned>(expect), static_cast<int>((now - t0) / 1000));
            last_log = now;
        }
    }
    ESP_LOGI(TAG, "Read done %u bytes %dms", static_cast<unsigned>(body.size()),
             static_cast<int>((esp_timer_get_time() - t0) / 1000));
    return body;
}

bool ParsePushResourceObject(const cJSON* obj, CloudPushResource& out) {
    if (obj == nullptr || !cJSON_IsObject(obj)) {
        return false;
    }

    const cJSON* resource_type = cJSON_GetObjectItemCaseSensitive(obj, "resourceType");
    const cJSON* task_id = cJSON_GetObjectItemCaseSensitive(obj, "taskId");
    const cJSON* resource_id = cJSON_GetObjectItemCaseSensitive(obj, "resourceId");
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(obj, "name");
    const cJSON* resource_name = cJSON_GetObjectItemCaseSensitive(obj, "resourceName");
    const cJSON* download_url = cJSON_GetObjectItemCaseSensitive(obj, "downloadUrl");
    const cJSON* cover_image_url = cJSON_GetObjectItemCaseSensitive(obj, "coverImageUrl");
    const cJSON* file_size = cJSON_GetObjectItemCaseSensitive(obj, "fileSize");
    const cJSON* sha256 = cJSON_GetObjectItemCaseSensitive(obj, "sha256");
    const cJSON* format = cJSON_GetObjectItemCaseSensitive(obj, "format");
    const cJSON* page_count = cJSON_GetObjectItemCaseSensitive(obj, "pageCount");
    const cJSON* font_style = cJSON_GetObjectItemCaseSensitive(obj, "fontStyle");
    const cJSON* font_weight = cJSON_GetObjectItemCaseSensitive(obj, "fontWeight");

    if (!cJSON_IsString(resource_type) || resource_type->valuestring == nullptr ||
        !cJSON_IsString(task_id) || task_id->valuestring == nullptr ||
        task_id->valuestring[0] == '\0' || !cJSON_IsString(name) || name->valuestring == nullptr ||
        name->valuestring[0] == '\0' || !cJSON_IsString(download_url) ||
        download_url->valuestring == nullptr || download_url->valuestring[0] == '\0') {
        return false;
    }

    const PushResourceType type = ParseResourceType(resource_type->valuestring);
    if (type == PushResourceType::kUnknown) {
        return false;
    }

    out = CloudPushResource{};
    out.type = type;
    out.task_id = task_id->valuestring;
    if (cJSON_IsString(resource_id) && resource_id->valuestring != nullptr) {
        out.resource_id = resource_id->valuestring;
    }
    out.name = name->valuestring;
    if (cJSON_IsString(resource_name) && resource_name->valuestring != nullptr) {
        out.resource_name = resource_name->valuestring;
    }
    out.download_url = download_url->valuestring;
    if (cJSON_IsString(cover_image_url) && cover_image_url->valuestring != nullptr &&
        cover_image_url->valuestring[0] != '\0') {
        out.cover_image_url = cover_image_url->valuestring;
    }
    ParseUint64Field(file_size, &out.file_size);
    if (cJSON_IsString(sha256) && sha256->valuestring != nullptr) {
        out.sha256 = sha256->valuestring;
    }
    if (cJSON_IsString(format) && format->valuestring != nullptr) {
        out.format = format->valuestring;
    }
    if (cJSON_IsNumber(page_count)) {
        out.page_count = page_count->valueint;
    } else {
        uint64_t pc = 0;
        if (ParseUint64Field(page_count, &pc) && pc <= static_cast<uint64_t>(INT_MAX)) {
            out.page_count = static_cast<int>(pc);
        }
    }
    if (cJSON_IsString(font_style) && font_style->valuestring != nullptr) {
        out.font_style = font_style->valuestring;
    }
    if (cJSON_IsNumber(font_weight)) {
        out.font_weight = font_weight->valueint;
    }
    return true;
}

CloudBookTask ToBookTask(const CloudPushResource& item) {
    CloudBookTask t;
    t.task_id = item.task_id;
    t.book_id = item.resource_id;
    t.name = item.name;
    if (!item.resource_name.empty()) {
        t.title = item.resource_name;
    } else {
        t.title = TitleFromPath(item.name.c_str());
        if (t.title.empty()) {
            t.title = item.name;
        }
    }
    t.format = item.format;
    t.download_url = item.download_url;
    t.cover_image_url = item.cover_image_url;
    t.file_size = item.file_size;
    t.sha256 = item.sha256;
    t.page_count = item.page_count;
    return t;
}

CloudWallpaperTask ToWallpaperTask(const CloudPushResource& item) {
    CloudWallpaperTask t;
    t.task_id = item.task_id;
    t.badge_image_id = item.resource_id;
    t.image_name = item.name;
    t.original_filename = item.name;
    t.download_url = item.download_url;
    t.file_size = item.file_size;
    t.sha256 = item.sha256;
    return t;
}

const char* CloudPushResource::TypeLabel() const {
    switch (type) {
        case PushResourceType::kBook:
            return Lang::Strings::CLOUD_TYPE_BOOK;
        case PushResourceType::kBadge:
            return Lang::Strings::CLOUD_TYPE_WALLPAPER;
        case PushResourceType::kFont:
            return Lang::Strings::CLOUD_TYPE_FONT;
        default:
            return Lang::Strings::COMMON_UNKNOWN;
    }
}

const std::string& CloudPushResource::CoverThumbUrl() const {
    return cover_image_url;
}

bool CloudPushResource::HasCoverThumb() const {
    return (type == PushResourceType::kBook || type == PushResourceType::kBadge ||
            type == PushResourceType::kFont) &&
           !cover_image_url.empty();
}

std::string CloudPushResource::LocalPath() const {
    if (type == PushResourceType::kBook) {
        return ToBookTask(*this).LocalPath();
    }
    if (type == PushResourceType::kBadge) {
        return ToWallpaperTask(*this).LocalPath();
    }
    if (type == PushResourceType::kFont) {
        std::string file = SanitizeBasename(name);
        if (file.empty()) {
            file = resource_id.empty() ? "font.ef" : ("font-" + resource_id + ".ef");
        }
        // .ttf/.otf → fonts_ttf（供阅读设置「导入 TTF」）；.ef 等 → fonts
        const char* dir = IsTtfOrOtfBasename(file) ? SD_PATH_FONT_SRC : SD_PATH_FONTS;
        std::string path = dir;
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path.append(file);
        return path;
    }
    return {};
}

bool CloudPushResource::IsDownloaded() const {
    struct stat st {};
    const std::string path = LocalPath();
    if (path.empty() || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return st.st_size > 0;
}

bool FetchPushResources(std::vector<CloudPushResource>& out, std::string& err_out,
                        DownloadGate* gate) {
    out.clear();
    err_out.clear();

    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = Lang::Strings::CLOUD_CANCELLED;
        return false;
    }
    ESP_LOGI(TAG, "push resources: ensure net");
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::CLOUD_NET_NOT_READY;
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = Lang::Strings::CLOUD_CANCELLED;
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
    Http* http_raw = http.get();
    if (gate != nullptr) {
        if (!gate->BindHttp(std::move(http))) {
            err_out = Lang::Strings::CLOUD_CANCELLED;
            return false;
        }
    }

    const std::string url = api::PushResourcesNextUrl();
    if (url.empty()) {
        ESP_LOGI(TAG, "skip push resources: cloud endpoints blank");
        return true;
    }
    http_raw->SetTimeout(kHttpTimeoutMs);
    api::ApplyCommonHeaders(http_raw);
    http_raw->SetContent(std::string());
    api::LogHttpRequest(TAG, "POST", url);
    ESP_LOGI(TAG, "push resources: Open begin timeout=%dms", kHttpTimeoutMs);
    if (!http_raw->Open("POST", url)) {
        if (gate != nullptr) {
            gate->CloseHttp();
        }
        err_out = gate != nullptr && gate->IsCancelled() ? Lang::Strings::CLOUD_CANCELLED : Lang::Strings::CLOUD_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }

    const int status = http_raw->GetStatusCode();
    std::string read_err;
    const std::string body = ReadHttpBodyProgress(http_raw, kMaxTaskListBodyBytes, read_err);
    if (gate != nullptr) {
        gate->CloseHttp();
        if (gate->IsCancelled()) {
            err_out = Lang::Strings::CLOUD_CANCELLED;
            return false;
        }
    } else {
        http_raw->Close();
    }
    if (!read_err.empty() || body.empty()) {
        err_out = read_err.empty() ? Lang::Strings::CLOUD_REQUEST_FAIL : read_err;
        api::LogHttpResponse(TAG, status, err_out);
        return false;
    }
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(body));
    if (status < 200 || status >= 300) {
        err_out = Lang::Strings::CLOUD_REQUEST_FAIL;
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = Lang::Strings::CLOUD_JSON_PARSE_FAIL;
        return false;
    }

    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr && msg->valuestring[0] != '\0') {
            err_out = msg->valuestring;
        } else {
            err_out = Lang::Strings::CLOUD_API_ERROR;
        }
        cJSON_Delete(root);
        return false;
    }

    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (data == nullptr || cJSON_IsNull(data)) {
        cJSON_Delete(root);
        return true;
    }
    if (!cJSON_IsArray(data)) {
        err_out = Lang::Strings::CLOUD_DATA_FORMAT_ERR;
        cJSON_Delete(root);
        return false;
    }

    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, data) {
        CloudPushResource res;
        if (!ParsePushResourceObject(item, res)) {
            ESP_LOGW(TAG, "skip invalid push resource item");
            continue;
        }
        out.push_back(std::move(res));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "push resources: parsed n=%u", static_cast<unsigned>(out.size()));
    return true;
}

}  // namespace reader
