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
#include <cstdlib>
#include <new>
#include <climits>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cJSON.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"
#include "assets/lang_screen.h"

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

namespace {

// Only allowlisted format labels enter logs; URLs and their query strings never do.
const char* ResourceFormatForLog(const std::string& value) {
    const size_t end = value.find_first_of("?#");
    const std::string path = value.substr(0, end);
    const size_t dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    const char* known[] = {"A2I1", "A2L8", "PNG", "JPEG", "JPG", "EBOOK_V1",
                           "EBOOK", "EPUB", "TXT", "PDF", "TTF", "OTF", "WOFF", "WOFF2"};
    for (const char* label : known) {
        if (strcasecmp(value.c_str(), label) == 0 || strcasecmp(ext.c_str(), label) == 0)
            return label;
    }
    return "unknown";
}

std::string ResourceNameForLog(const std::string& name) {
    // Retain only a basename, without query strings, paths or control characters.
    std::string result = name.substr(0, name.find_first_of("?#"));
    const size_t slash = result.find_last_of("/\\");
    if (slash != std::string::npos) result.erase(0, slash + 1);
    for (char& c : result) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }
    return result;
}

void LogResourceSummary(const cJSON* item, const CloudPushResource& res, size_t index) {
    const char* type = res.type == PushResourceType::kBook ? "BOOK" :
                       res.type == PushResourceType::kBadge ? "BADGE" : "FONT";
    const char* format = ResourceFormatForLog(res.format);
    if (std::strcmp(format, "unknown") == 0) format = ResourceFormatForLog(res.name);
    const cJSON* width = cJSON_GetObjectItemCaseSensitive(item, "coverWidth");
    const cJSON* height = cJSON_GetObjectItemCaseSensitive(item, "coverHeight");
    const int w = cJSON_IsNumber(width) && width->valueint > 0 ? width->valueint : 0;
    const int h = cJSON_IsNumber(height) && height->valueint > 0 ? height->valueint : 0;
    ESP_LOGI(TAG, "list item=%u type=%s name=%s file_format=%s cover_format=%s cover_size=%dx%d",
             static_cast<unsigned>(index), type, ResourceNameForLog(res.name).c_str(),
             format, ResourceFormatForLog(res.cover_image_url), w, h);
}

}  // namespace

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
        if (!SdIsValidFileName(file.c_str())) return {};
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

namespace {
// 网络输入只保留一个块；资源对象逐条交给现有 cJSON 校验。
class HttpJsonStream {
public:
    using Ch = char;
    explicit HttpJsonStream(Http* http) : http_(http) {}
    Ch Peek() {
        if (position_ == size_ && !ended_) {
            const int count = http_->Read(buffer_, sizeof(buffer_));
            failed = count < 0;
            ended_ = count <= 0;
            size_ = count > 0 ? static_cast<size_t>(count) : 0;
            position_ = 0;
        }
        return position_ < size_ ? buffer_[position_] : '\0';
    }
    Ch Take() {
        const Ch value = Peek();
        if (position_ < size_) { ++position_; ++offset_; }
        return value;
    }
    size_t Tell() const { return offset_; }
    bool AtEnd() { return Peek() == '\0' && ended_ && position_ == size_; }
    bool failed = false; // 网络读取错误
private:
    Http* http_;
    char buffer_[1024]{};
    size_t position_ = 0, size_ = 0, offset_ = 0;
    bool ended_ = false;
};

class PushListHandler {
public:
    explicit PushListHandler(std::vector<CloudPushResource>& out) : out_(out) {}
    bool Parse(HttpJsonStream& stream) {
        Space(stream);
        if (stream.Take() != '{') return false;
        root_object = true;
        Space(stream);
        if (stream.Peek() == '}') stream.Take();
        else for (;;) {
            auto key = Value(stream);
            if (!key || !cJSON_IsString(key.get())) return false;
            Space(stream);
            if (stream.Take() != ':') return false;
            Space(stream);
            if (std::strcmp(key->valuestring, "data") == 0 && stream.Peek() == '[') {
                valid_data = true;
                if (!Items(stream)) return false;
            } else {
                auto value = Value(stream);
                if (!value) return false;
                if (std::strcmp(key->valuestring, "code") == 0) {
                    has_code = cJSON_IsNumber(value.get());
                    if (has_code) code = value->valuedouble;
                } else if (std::strcmp(key->valuestring, "msg") == 0 && cJSON_IsString(value.get())) {
                    message = value->valuestring;
                } else if (std::strcmp(key->valuestring, "data") == 0) {
                    valid_data = cJSON_IsNull(value.get());
                }
            }
            Space(stream);
            const char separator = stream.Take();
            if (separator == '}') break;
            if (separator != ',') return false;
        }
        Space(stream);
        return stream.AtEnd() && !stream.failed;
    }
    bool root_object = false; // 顶层响应类型
    bool has_code = false; // code 必须为数值
    double code = -1;
    bool valid_data = true; // 缺省或 null 表示没有任务
    std::string message;
private:
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    static void Space(HttpJsonStream& stream) {
        while (stream.Peek() == ' ' || stream.Peek() == '\t' || stream.Peek() == '\r' || stream.Peek() == '\n')
            stream.Take();
    }
    // 只识别一个 JSON 值的边界，语法、转义及嵌套由 cJSON 统一校验。
    static Json Value(HttpJsonStream& stream) {
        Space(stream);
        std::string raw;
        unsigned depth = 0;
        bool quoted = false, escaped = false;
        for (;;) {
            const char ch = stream.Peek();
            if (!ch) break;
            if (!quoted && depth == 0 && (ch == ',' || ch == ']' || ch == '}' ||
                ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) break;
            raw.push_back(stream.Take());
            if (quoted) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') { quoted = false; if (depth == 0) break; }
            } else if (ch == '"') quoted = true;
            else if (ch == '{' || ch == '[') ++depth;
            else if (ch == '}' || ch == ']') { if (--depth == 0) break; }
        }
        return Json(cJSON_ParseWithLengthOpts(raw.c_str(), raw.size() + 1, nullptr, true), cJSON_Delete);
    }
    bool Items(HttpJsonStream& stream) {
        stream.Take();
        Space(stream);
        if (stream.Peek() == ']') { stream.Take(); return true; }
        for (;;) {
            auto item = Value(stream);
            if (!item) return false;
            CloudPushResource resource;
            if (ParsePushResourceObject(item.get(), resource)) {
                LogResourceSummary(item.get(), resource, out_.size());
                out_.push_back(std::move(resource));
            } else ESP_LOGW(TAG, "skip invalid push resource item");
            Space(stream);
            const char separator = stream.Take();
            if (separator == ']') return true;
            if (separator != ',') return false;
        }
    }
    std::vector<CloudPushResource>& out_;
};
} // namespace

bool FetchPushResources(std::vector<CloudPushResource>& out, std::string& err_out,
                        DownloadGate* gate) try {
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
    DownloadGate::Binding binding;
    Http* http_raw = http.get();
    if (gate != nullptr) {
        if (!gate->BindHttp(http.get())) {
            err_out = Lang::Strings::CLOUD_CANCELLED;
            return false;
        }
    }
    binding.gate = gate;

    const std::string url = api::PushResourcesNextUrl();
    if (url.empty()) {
        ESP_LOGI(TAG, "skip push resources: cloud endpoints blank");
        return true;
    }
    http_raw->SetTimeout(kHttpTimeoutMs);
    api::ApplyCommonHeaders(http_raw);
    http_raw->SetContent(std::string());
    ESP_LOGI(TAG, "push resources: Open begin timeout=%dms", kHttpTimeoutMs);
    if (!http_raw->Open("POST", url)) {
        if (gate != nullptr) {
            gate->CloseHttp();
        }
        err_out = gate != nullptr && gate->IsCancelled() ? Lang::Strings::CLOUD_CANCELLED : Lang::Strings::CLOUD_FETCH_FAIL;
        return false;
    }

    const int status = http_raw->GetStatusCode();
    HttpJsonStream stream(http_raw);
    PushListHandler handler(out);
    const bool parsed = handler.Parse(stream);
    if (gate != nullptr) gate->CloseHttp();
    else http_raw->Close();
    ESP_LOGI(TAG, "list response status=%d bytes=%u", status, static_cast<unsigned>(stream.Tell()));
    if (gate != nullptr && gate->IsCancelled()) err_out = Lang::Strings::CLOUD_CANCELLED;
    else if (status < 200 || status >= 300) {
        err_out = std::string(Lang::Strings::CLOUD_FETCH_FAIL) + " (HTTP " + std::to_string(status) + ")";
    } else if (stream.failed) err_out = Lang::Strings::CLOUD_FETCH_FAIL;
    else if (!parsed || !handler.root_object) err_out = Lang::Strings::CLOUD_JSON_PARSE_FAIL;
    else if (!handler.has_code || handler.code != 0) {
        err_out = handler.message.empty() ? Lang::Strings::CLOUD_API_ERROR : handler.message;
        if (handler.has_code) {
            const int code = handler.code >= INT_MAX ? INT_MAX : handler.code <= INT_MIN ? INT_MIN :
                static_cast<int>(handler.code);
            err_out += " (code " + std::to_string(code) + ")";
        }
    } else if (!handler.valid_data) err_out = Lang::Strings::CLOUD_DATA_FORMAT_ERR;
    if (!err_out.empty()) { out.clear(); return false; }
    ESP_LOGI(TAG, "push resources: parsed n=%u", static_cast<unsigned>(out.size()));
    return true;
} catch (const std::bad_alloc&) {
    out.clear();
    err_out = Lang::Strings::BOOK_OUT_OF_MEMORY;
    return false;
}

}  // namespace reader
