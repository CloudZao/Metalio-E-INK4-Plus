#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <mbedtls/build_info.h>
#include <dirent.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include "private/elf_validate.h"
#include <psa/crypto.h>
#include <sys/stat.h>
#include <unistd.h>
#include "infra/storage/sd_file_commit.h"
#include "app_store.h"
#include "app_api.h"
#include "app_store_icons.h"
#include "assets/lang_app_store.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "infra/network/http/api_http.h"
#include "infra/network/http/http_download_file.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "infra/platform/system_info.h"

extern "C" {
#define XZ_APP_RUNTIME_MATH(name, result, args) result name args;
#include "app_runtime_math.def"
#undef XZ_APP_RUNTIME_MATH
}

static const esp_elf_symbol_table_t kAppStoreRuntimeMath[] = {
#define XZ_APP_RUNTIME_MATH(name, result, args) ESP_ELFSYM_EXPORT(name),
#include "app_runtime_math.def"
#undef XZ_APP_RUNTIME_MATH
    ESP_ELFSYM_END};
constexpr const char* TAG = "AppStore";
constexpr size_t kAppStoreMaxImage = 32 * 1024 * 1024;
constexpr size_t kAppStorePageSize = 16;
using AppStoreJson = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

enum class AppStoreManifestStatus {
    Missing,   // 尚无安装清单
    Invalid,   // 清单内容无效
    IoError,   // 清单读写失败
    NoMemory,  // 无法分配读取缓冲
    Ready      // 清单读取成功
};

struct AppStoreFailure {
    int status = -1;           // HTTP 状态，负值表示传输失败
    unsigned retry_after = 0;  // 服务端建议等待秒数
    bool retryable = true;     // 是否允许有限重试
    std::string code;          // 已校验的协议错误码
};

struct AppStoreFileSource {
    FILE* file;     // 校验后保持打开的 ELF 文件
    size_t length;  // 清单声明且已校验的文件长度
};

static const std::string kAppStoreRoot = SD_PATH_APPS;
static std::mutex app_store_s_storage;

static bool AppStoreIsValidAppId(const std::string& s) {
    if (s.empty() || s.size() > 48 || s[0] < 'a' || s[0] > 'z') {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool AppStoreIsLowercaseHex(const std::string& s, size_t length) {
    return s.size() == length && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

static bool AppStoreParseVersion(const std::string& s, unsigned parts[3]) {
    if (s.size() > 20) {
        return false;
    }
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t begin = pos;
        parts[i] = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (pos - begin >= 6) {
                return false;
            }
            parts[i] = parts[i] * 10 + s[pos++] - '0';
        }
        if (pos == begin || (pos - begin > 1 && s[begin] == '0')) {
            return false;
        }
        if (i < 2 && (pos >= s.size() || s[pos++] != '.')) {
            return false;
        }
    }
    return pos == s.size();
}

static bool AppStoreReadRequiredText(cJSON* root, const char* key, std::string& out, size_t max) {
    auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) > max) {
        return false;
    }
    out = v->valuestring;
    return true;
}

static bool AppStoreReadOptionalText(cJSON* root, const char* key, std::string& out, size_t max) {
    if (!cJSON_GetObjectItemCaseSensitive(root, key)) {
        out.clear();
        return true;
    }
    return AppStoreReadRequiredText(root, key, out, max);
}

static bool AppStoreValidInstallName(const std::string& name) {
    return !name.empty() && name.size() <= 72 && name.front() != '.' && SdIsValidFileName(name.c_str());
}

static std::string AppStoreInstallName(const AppStoreItem& item) {
    if (!item.installed_name.empty()) {
        return item.installed_name;
    }
    std::string name = item.name.empty() ? item.name_en : item.name;
    name.resize(SdUtf8PrefixLength(name.c_str(), 72));
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
        name.pop_back();
    }
    if (name.empty()) {
        name = "app";
    }
    if (name.front() == '.') {
        name.front() = '_';
    }
    return name;
}

static std::string AppStoreInstallDirectory(const AppStoreItem& item) {
    return kAppStoreRoot + "/" + AppStoreInstallName(item);
}

static std::string AppStoreImagePath(const AppStoreItem& item) {
    return AppStoreInstallDirectory(item) + "/" + AppStoreInstallName(item) + ".elf";
}

static bool AppStoreParseManifestItem(cJSON* root, AppStoreItem& item) {
    std::string target;
    unsigned parts[3];
    auto* abi = cJSON_GetObjectItemCaseSensitive(root, "abiVersion");
    auto* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!AppStoreReadRequiredText(root, "id", item.id, 32) || !AppStoreIsLowercaseHex(item.id, 32) ||
        !AppStoreReadRequiredText(root, "appId", item.app_id, 48) || !AppStoreIsValidAppId(item.app_id) ||
        !AppStoreReadRequiredText(root, "name", item.name, 128) || item.name.empty() ||
        !AppStoreReadRequiredText(root, "nameEn", item.name_en, 192) || item.name_en.empty() ||
        !AppStoreReadRequiredText(root, "description", item.description, 1024) ||
        !AppStoreReadRequiredText(root, "descriptionEn", item.description_en, 1024) ||
        !AppStoreReadOptionalText(root, "installedName", item.installed_name, 72) ||
        (!item.installed_name.empty() && !AppStoreValidInstallName(item.installed_name)) ||
        !AppStoreReadOptionalText(root, "summary", item.summary, 256) ||
        !AppStoreReadOptionalText(root, "summaryEn", item.summary_en, 256) ||
        !AppStoreReadOptionalText(root, "category", item.category, 96) ||
        !AppStoreReadOptionalText(root, "categoryEn", item.category_en, 96) ||
        !AppStoreReadOptionalText(root, "languages", item.languages, 263) ||
        !AppStoreReadOptionalText(root, "provider", item.provider, 128) ||
        !AppStoreReadOptionalText(root, "releaseNotes", item.release_notes, 1024) ||
        !AppStoreReadOptionalText(root, "releaseNotesEn", item.release_notes_en, 1024) ||
        !AppStoreReadOptionalText(root, "uploadedAt", item.uploaded_at, 64) ||
        !AppStoreReadRequiredText(root, "version", item.version, 20) || !AppStoreParseVersion(item.version, parts) ||
        !AppStoreReadRequiredText(root, "sha256", item.sha256, 64) || !AppStoreIsLowercaseHex(item.sha256, 64) ||
        !AppStoreReadRequiredText(root, "target", target, 32) || target != CONFIG_IDF_TARGET || !cJSON_IsNumber(abi) ||
        abi->valuedouble != 1 || !cJSON_IsNumber(size) || size->valuedouble < 1 ||
        size->valuedouble > kAppStoreMaxImage || size->valuedouble != static_cast<size_t>(size->valuedouble)) {
        return false;
    }
    item.size = static_cast<size_t>(size->valuedouble);
    auto* icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    if (icon) {
        if (!cJSON_IsString(icon) || !icon->valuestring || strlen(icon->valuestring) != 2752) {
            return false;
        }
        unsigned char data[2064];
        size_t length = 0;
        if (mbedtls_base64_decode(data, sizeof(data), &length,
                                  reinterpret_cast<const unsigned char*>(icon->valuestring), 2752) != 0) {
            return false;
        }
        item.icon = AppStoreIconsDecode(data, length);
        if (!item.icon) {
            return false;
        }
    }
    return true;
}

static AppStoreJson AppStoreSerializeManifestItem(const AppStoreItem& item) {
    AppStoreJson j(cJSON_CreateObject(), cJSON_Delete);
    if (!j) {
        return j;
    }
    if (!cJSON_AddStringToObject(j.get(), "id", item.id.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "appId", item.app_id.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "installedName", AppStoreInstallName(item).c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "name", item.name.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "nameEn", item.name_en.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "description", item.description.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "descriptionEn", item.description_en.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "summary", item.summary.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "summaryEn", item.summary_en.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "category", item.category.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "categoryEn", item.category_en.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "languages", item.languages.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "provider", item.provider.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "releaseNotes", item.release_notes.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "releaseNotesEn", item.release_notes_en.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "uploadedAt", item.uploaded_at.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "version", item.version.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "sha256", item.sha256.c_str())) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddStringToObject(j.get(), "target", CONFIG_IDF_TARGET)) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddNumberToObject(j.get(), "abiVersion", 1)) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!cJSON_AddNumberToObject(j.get(), "size", item.size)) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    return j;
}

static bool AppStoreNetworkReady(std::string& error, const std::atomic<bool>& cancelled) {
    auto& board = Board::GetInstance();
    const bool ready = !board.GetNetworkErrorText(nullptr) && board.EnsureNetworkReady(20000);
    if (cancelled.load()) {
        ESP_LOGI(TAG, "operation=network result=cancelled");
        error = LangString_STORE_CANCELLED;
        return false;
    }
    if (ready) {
        return true;
    }
    ESP_LOGW(TAG, "operation=network stage=ready result=unavailable timeout_ms=20000");
    error = LangString_STORE_NETWORK_ERROR;
    return false;
}

static bool AppStorePrepareStorage() {
    return SdEnsureDirectory(SD_PATH_APPS) && SdEnsureDirectory(SD_PATH_APP_STORE_TEMP);
}

static std::string AppStoreWorkDirectory(const AppStoreItem& item) {
    if (!AppStoreIsValidAppId(item.app_id)) {
        errno = EINVAL;
        return {};
    }
    // 名称只作可读标签，稳定 ID 避免重名；改名后仍按 ID 查找续传数据。
    const std::string suffix = "--" + item.app_id;
    DIR* entries = opendir(SD_PATH_APP_STORE_TEMP);
    if (!entries) {
        return {};
    }
    std::string found;
    while (auto* entry = readdir(entries)) {
        const std::string name = entry->d_name;
        if (SdIsValidFileName(name.c_str()) && name.size() > suffix.size() && name.ends_with(suffix)) {
            found = std::string(SD_PATH_APP_STORE_TEMP) + "/" + name;
            struct stat st{};
            if (stat(found.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                found.clear();
                continue;
            }
            break;
        }
    }
    closedir(entries);
    return found.empty() ? std::string(SD_PATH_APP_STORE_TEMP) + "/" + AppStoreInstallName(item) + suffix : found;
}

static AppStoreManifestStatus AppStoreReadManifest(const std::string& path, AppStoreItem& item, int& code) {
    code = 0;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        code = errno;
        return code == ENOENT ? AppStoreManifestStatus::Missing : AppStoreManifestStatus::IoError;
    }
    std::unique_ptr<char[]> data(new (std::nothrow) char[16385]);
    if (!data) {
        code = ENOMEM;
        fclose(f);
        return AppStoreManifestStatus::NoMemory;
    }
    const size_t n = fread(data.get(), 1, 16385, f);
    bool ok = !ferror(f);
    if (!ok) {
        code = errno;
    }
    if (fclose(f) != 0 && ok) {
        code = errno;
        ok = false;
    }
    if (!ok) {
        return AppStoreManifestStatus::IoError;
    }
    if (n >= 16385) {
        return AppStoreManifestStatus::Invalid;
    }
    data[n] = 0;
    AppStoreJson j(cJSON_ParseWithLengthOpts(data.get(), n + 1, nullptr, true), cJSON_Delete);
    return j && AppStoreParseManifestItem(j.get(), item) ? AppStoreManifestStatus::Ready
                                                         : AppStoreManifestStatus::Invalid;
}

static bool AppStoreFinishAndVerifyHash(psa_hash_operation_t& op, const std::string& expected) {
    unsigned char hash[32];
    size_t length = 0;
    const bool ok = psa_hash_finish(&op, hash, sizeof(hash), &length) == PSA_SUCCESS && length == sizeof(hash);
    psa_hash_abort(&op);
    if (!ok) {
        return false;
    }
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return expected == hex;
}

static bool AppStoreVerifyFileDigest(FILE* f, size_t expected_size, const std::string& expected_hash,
                                     const std::atomic<bool>* cancelled = nullptr) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    unsigned char buf[4096];
    size_t total = 0;
    bool ok = true;
    for (;;) {
        if (cancelled && cancelled->load()) {
            ok = false;
            break;
        }
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (!n) {
            ok = !ferror(f);
            break;
        }
        if (total > expected_size || n > expected_size - total) {
            ok = false;
            break;
        }
        total += n;
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
    }
    if (ok && total == expected_size) {
        return AppStoreFinishAndVerifyHash(op, expected_hash);
    }
    psa_hash_abort(&op);
    return false;
}

static bool AppStoreImageMatches(const AppStoreItem& item, const std::string& directory) {
    if (directory != AppStoreInstallName(item)) {
        return false;
    }
    const auto dir = kAppStoreRoot + "/" + directory;
    const auto image = dir + "/" + AppStoreInstallName(item) + ".elf";
    const auto saved = dir + "/" + item.sha256 + ".elf";
    struct stat st{};
    const bool have_saved = stat(saved.c_str(), &st) == 0;
    if (stat(image.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == item.size) {
        if (!have_saved) {
            return true;
        }
        FILE* file = fopen(image.c_str(), "rb");
        const bool valid = file && AppStoreVerifyFileDigest(file, item.size, item.sha256);
        if (file) {
            fclose(file);
        }
        if (valid) {
            return true;
        }
    }
    FILE* file = fopen(saved.c_str(), "rb");
    const bool valid = file && AppStoreVerifyFileDigest(file, item.size, item.sha256);
    if (file) {
        fclose(file);
    }
    if (!valid) {
        return false;
    }
    if ((unlink(image.c_str()) != 0 && errno != ENOENT) || rename(saved.c_str(), image.c_str()) != 0) {
        ESP_LOGE(TAG, "operation=scan stage=image_recover result=failed errno=%d", errno);
        return false;
    }
    return true;
}

static void AppStoreCleanVersions(const std::string& dir, const std::string& keep) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        ESP_LOGW(TAG, "operation=cleanup stage=open_directory result=failed errno=%d", errno);
        return;
    }
    while (auto* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() == 68 && name.substr(64) == ".elf" && AppStoreIsLowercaseHex(name.substr(0, 64), 64) &&
            name != keep && unlink((dir + "/" + name).c_str()) != 0) {
            ESP_LOGW(TAG, "operation=cleanup stage=unlink result=failed errno=%d", errno);
        }
    }
    closedir(d);
}

static std::string AppStoreOrigin(const std::string& server) {
    return server.substr(0, server.find('/', server.find("://") + 3));
}

static std::string AppStoreResponseHeader(Http* http, const char* lower, const char* canonical) {
    std::string value = http->GetResponseHeader(lower);
    return value.empty() ? http->GetResponseHeader(canonical) : value;
}

static bool AppStoreSafeToken(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
               std::string::npos;
}

static void AppStoreSetRequestHeaders(Http* http, bool binary = false) {
    char id[40];
    snprintf(id, sizeof(id), "%08lx-%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random());
    http->SetHeader("X-Request-Id", id);
    http->SetHeader("Accept", binary ? "application/octet-stream" : "application/json");
    http->SetHeader("Accept-Language", LangCurrent());
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Connection", "close");
}

static unsigned AppStoreRetryAfter(Http* http) {
    const auto value = AppStoreResponseHeader(http, "retry-after", "Retry-After");
    unsigned seconds = 0;
    if (value.empty() || value.size() > 9) {
        return 0;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return 0;
        }
        seconds = seconds * 10 + unsigned(c - '0');
    }
    return seconds;
}

static bool AppStoreWaitRetry(unsigned attempt, unsigned seconds, const std::atomic<bool>& cancelled) {
    // 长等待交还用户，避免商店任务长期占用资源；不提前重试。
    if (seconds > 60) {
        return false;
    }
    unsigned ticks = std::max(seconds * 10, (1u << attempt) * 10) + esp_random() % 5;
    for (unsigned i = 0; i < ticks && !cancelled.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !cancelled.load();
}

static bool AppStoreReadBody(Http* http, std::string& body, size_t limit, const std::atomic<bool>& cancelled) {
    char buffer[2048];
    while (!cancelled.load()) {
        int n = http->Read(buffer, sizeof(buffer));
        if (n < 0) {
            return false;
        }
        if (!n) {
            return true;
        }
        if (size_t(n) > limit - body.size()) {
            return false;
        }
        body.append(buffer, n);
    }
    return false;
}

static void AppStoreParseHttpFailure(const std::string& body, AppStoreFailure& failure, std::string& error) {
    failure.retryable = failure.status < 0 || failure.status == 429 || failure.status >= 500;
    AppStoreJson root(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true), cJSON_Delete);
    auto* status = cJSON_GetObjectItemCaseSensitive(root.get(), "status");
    auto* retryable = cJSON_GetObjectItemCaseSensitive(root.get(), "retryable");
    std::string request_id;
    if (cJSON_IsNumber(status) && status->valuedouble == failure.status &&
        AppStoreReadRequiredText(root.get(), "code", failure.code, 64) && AppStoreSafeToken(failure.code)) {
        if (cJSON_IsBool(retryable)) {
            failure.retryable = failure.retryable && cJSON_IsTrue(retryable);
        }
        AppStoreReadRequiredText(root.get(), "requestId", request_id, 64);
    } else {
        failure.code.clear();
    }
    const char* known = "unknown";
    for (const char* code : {"INVALID_REQUEST", "DEVICE_FORBIDDEN", "APP_NOT_AVAILABLE", "RELEASE_NOT_FOUND",
                             "CATALOG_REVISION_EXPIRED", "RELEASE_WITHDRAWN", "PACKAGE_CHANGED", "RANGE_INVALID",
                             "DEVICE_UNSUPPORTED", "INTERNAL_ERROR", "SERVICE_UNAVAILABLE"}) {
        if (failure.code == code) {
            known = code;
            break;
        }
    }
    error = LangString_STORE_NETWORK_ERROR;
    switch (failure.status) {
        case 400:
            error = LangString_STORE_INVALID;
            break;
        case 403:
            error = LangString_STORE_FORBIDDEN;
            break;
        case 404:
            error = LangString_STORE_UNAVAILABLE;
            break;
        case 409:
            error = LangString_STORE_CATALOG_EXPIRED;
            break;
        case 410:
            error = LangString_STORE_WITHDRAWN;
            break;
        case 412:
            error = LangString_STORE_PACKAGE_CHANGED;
            break;
        case 416:
            error = LangString_STORE_RANGE_ERROR;
            break;
        case 422:
            error = LangString_STORE_UNSUPPORTED;
            break;
        case 429:
        case 503:
            error = LangString_STORE_SERVICE_BUSY;
            break;
    }
    if (failure.status == 400 || failure.status >= 500) {
        ESP_LOGE(TAG, "operation=http result=failed http_status=%d code=%s request_id=%s", failure.status, known,
                 AppStoreSafeToken(request_id) ? request_id.c_str() : "unavailable");
    } else {
        ESP_LOGW(TAG, "operation=http result=failed http_status=%d code=%s request_id=%s", failure.status, known,
                 AppStoreSafeToken(request_id) ? request_id.c_str() : "unavailable");
    }
}

static std::string AppStoreEscapeQuery(const std::string& value) {
    std::string out;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.') {
            out += c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string AppStoreDeviceQuery(const AppStoreItem& item) {
    return "?boardName=" + AppStoreEscapeQuery(BOARD_NAME) +
           "&chipName=" + AppStoreEscapeQuery(SystemInfo::GetChipModelName()) +
           "&mac=" + AppStoreEscapeQuery(ApiDeviceId()) + "&appId=" + AppStoreEscapeQuery(item.app_id);
}

static AppStoreJson AppStoreRequest(const AppStoreItem* item = nullptr) {
    AppStoreJson root(cJSON_CreateObject(), cJSON_Delete);
    auto* device = root ? cJSON_AddObjectToObject(root.get(), "device") : nullptr;
    const std::string chip = SystemInfo::GetChipModelName();
    const auto& mac = ApiDeviceId();
    unsigned parts[3];
    if (!AppStoreParseVersion(esp_app_get_description()->version, parts) || mac.size() != 17 ||
        mac == "00:00:00:00:00:00" || mac == "ff:ff:ff:ff:ff:ff" || !device ||
        !cJSON_AddNumberToObject(root.get(), "schemaVersion", 1) ||
        !cJSON_AddStringToObject(device, "boardName", BOARD_NAME) ||
        !cJSON_AddStringToObject(device, "chipName", chip.c_str()) ||
        !cJSON_AddStringToObject(device, "mac", mac.c_str()) ||
        !cJSON_AddStringToObject(device, "firmwareVersion", esp_app_get_description()->version) ||
        !cJSON_AddNumberToObject(device, "appAbiVersion", 1) ||
        (item && (!cJSON_AddStringToObject(root.get(), "appId", item->app_id.c_str()) ||
                  !cJSON_AddStringToObject(root.get(), "releaseId", item->id.c_str())))) {
        ESP_LOGE(TAG, "operation=request stage=device_or_json result=invalid");
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    return root;
}

static AppStoreJson AppStoreSendJsonRequestOnce(const std::string& server, const char* route, cJSON* request,
                                                std::string& error, const std::atomic<bool>& cancelled, bool get,
                                                AppStoreFailure& failure) {
    if (!request && !get) {
        error = LangString_STORE_MEMORY_ERROR;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!AppStoreNetworkReady(error, cancelled)) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    std::unique_ptr<char, decltype(&cJSON_free)> text(request ? cJSON_PrintUnformatted(request) : nullptr, cJSON_free);
    if (!get && (!text || strlen(text.get()) > 4096)) {
        error = LangString_STORE_INVALID;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGE(TAG, "operation=message stage=create_http result=failed");
        error = LangString_STORE_NETWORK_ERROR;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    AppStoreSetRequestHeaders(http.get());
    http->SetTimeout(15000);
    http->SetRequestDeadline(esp_timer_get_time() + 15000000);
    if (text) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(text.get()));
    }
    bool opened = http->Open(get ? "GET" : "POST", server + "/" + route);
    failure.status = opened ? http->GetStatusCode() : -1;
    failure.retry_after = AppStoreRetryAfter(http.get());
    std::string body;
    bool ok = opened && AppStoreReadBody(http.get(), body, failure.status == 200 ? 256 * 1024 : 4096, cancelled);
    const int code = http->GetLastError();
    if (cancelled.load()) {
        http->Cancel();
    }
    http->Close();
    if (cancelled.load()) {
        error = LangString_STORE_CANCELLED;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (failure.status != 200) {
        AppStoreParseHttpFailure(body, failure, error);
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    if (!ok) {
        ESP_LOGW(TAG, "operation=message stage=read result=failed transport_error=%d", code);
        error = LangString_STORE_NETWORK_ERROR;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    AppStoreJson response(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true), cJSON_Delete);
    auto* schema = cJSON_GetObjectItemCaseSensitive(response.get(), "schemaVersion");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1) {
        ESP_LOGE(TAG, "operation=message stage=json result=invalid");
        error = LangString_STORE_INVALID;
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    return response;
}

static AppStoreJson AppStoreSendJsonRequest(const std::string& server, const char* route, cJSON* request,
                                            std::string& error, const std::atomic<bool>& cancelled, bool get = false,
                                            AppStoreFailure* result_failure = nullptr) {
    for (unsigned attempt = 0; attempt < 4 && !cancelled.load(); ++attempt) {
        error.clear();
        AppStoreFailure failure;
        auto result = AppStoreSendJsonRequestOnce(server, route, request, error, cancelled, get, failure);
        if (result_failure) {
            *result_failure = failure;
        }
        if (result) {
            return result;
        }
        const bool transient =
            failure.status < 0 || failure.status == 200 || failure.status == 429 || failure.status >= 500;
        if (cancelled.load() || !transient || !failure.retryable || error == LangString_STORE_INVALID ||
            error == LangString_STORE_MEMORY_ERROR || attempt == 3 ||
            !AppStoreWaitRetry(attempt, failure.retry_after, cancelled)) {
            break;
        }
    }
    if (cancelled.load()) {
        error = LangString_STORE_CANCELLED;
    }
    return AppStoreJson(nullptr, cJSON_Delete);
}

static bool AppStoreParsePackageByteCount(const cJSON* value, size_t& size, size_t minimum = 1) {
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
        return false;
    }
    size_t result = 0;
    size_t digits = 0;
    for (const char* cursor = value->valuestring; *cursor; ++cursor) {
        if (++digits > 8 || *cursor < '0' || *cursor > '9') {
            return false;
        }
        const size_t digit = static_cast<size_t>(*cursor - '0');
        if (result > (kAppStoreMaxImage - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    if (result < minimum) {
        return false;
    }
    size = result;
    return true;
}

static bool AppStoreParseCatalogItem(cJSON* value, AppStoreItem& item) {
    if (!cJSON_IsObject(value)) {
        return false;
    }
    size_t package_size = 0;
    if (!AppStoreParsePackageByteCount(cJSON_GetObjectItemCaseSensitive(value, "size"), package_size)) {
        return false;
    }
    // 网络字符串长度转换为本地安装清单使用的数值格式。
    cJSON_DeleteItemFromObjectCaseSensitive(value, "size");
    if (!cJSON_AddNumberToObject(value, "size", package_size)) {
        return false;
    }
    // 本地清单的兼容字段由当前请求条件补齐。
    for (const char* key : {"description", "descriptionEn"}) {
        if (!cJSON_GetObjectItemCaseSensitive(value, key) && !cJSON_AddStringToObject(value, key, "")) {
            return false;
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(value, "target");
    cJSON_DeleteItemFromObjectCaseSensitive(value, "abiVersion");
    cJSON_DeleteItemFromObjectCaseSensitive(value, "id");
    if (!cJSON_GetObjectItemCaseSensitive(value, "target") &&
        !cJSON_AddStringToObject(value, "target", CONFIG_IDF_TARGET)) {
        return false;
    }
    if (!cJSON_GetObjectItemCaseSensitive(value, "abiVersion") && !cJSON_AddNumberToObject(value, "abiVersion", 1)) {
        return false;
    }
    auto* languages = cJSON_GetObjectItemCaseSensitive(value, "languages");
    if (cJSON_IsArray(languages)) {
        if (cJSON_GetArraySize(languages) < 1 || cJSON_GetArraySize(languages) > 8) {
            return false;
        }
        std::string joined;
        cJSON* language = nullptr;
        cJSON_ArrayForEach(language, languages) {
            if (!cJSON_IsString(language) || !language->valuestring || strlen(language->valuestring) > 32) {
                return false;
            }
            if (!joined.empty()) {
                joined += ',';
            }
            joined += language->valuestring;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(value, "languages");
        if (!cJSON_AddStringToObject(value, "languages", joined.c_str())) {
            return false;
        }
    }
    auto* release = cJSON_GetObjectItemCaseSensitive(value, "releaseId");
    if (!cJSON_IsString(release) || !cJSON_AddStringToObject(value, "id", release->valuestring)) {
        return false;
    }
    auto* icon = cJSON_DetachItemFromObjectCaseSensitive(value, "icon");
    AppStoreJson icon_owner(icon, cJSON_Delete);
    // 图标单独校验，异常时使用默认图标，不使整个目录失效。
    if (!AppStoreParseManifestItem(value, item)) {
        return false;
    }
    item.installed_name.clear();
    if (icon) {
        std::string data, format;
        auto* bytes = cJSON_GetObjectItemCaseSensitive(icon, "size");
        if (AppStoreReadRequiredText(icon, "format", format, 16) && format == "xzic" &&
            AppStoreReadRequiredText(icon, "data", data, 2752) && cJSON_IsNumber(bytes) && bytes->valuedouble == 2064) {
            unsigned char bitmap[2064];
            size_t length = 0;
            if (mbedtls_base64_decode(bitmap, sizeof(bitmap), &length,
                                      reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 0) {
                item.icon = AppStoreIconsDecode(bitmap, length);
            }
        }
        if (!item.icon) {
            ESP_LOGW(TAG, "operation=icon result=fallback");
        }
    }
    return true;
}

static size_t AppStoreSourceSize(void* context) {
    return static_cast<AppStoreFileSource*>(context)->length;
}

static int AppStoreSourceRead(void* context, size_t offset, void* buffer, size_t length) {
    auto* source = static_cast<AppStoreFileSource*>(context);
    if (offset > source->length || length > source->length - offset) {
        return -EINVAL;
    }
    return fseek(source->file, offset, SEEK_SET) == 0 && fread(buffer, 1, length, source->file) == length ? 0 : -EIO;
}

static bool AppStoreValidateImage(FILE* file, size_t length) {
    AppStoreFileSource context{file, length};
    const elf_source_t source{AppStoreSourceSize, AppStoreSourceRead, nullptr, &context};
    return esp_elf_validate_source(&source) == 0;
}

static bool AppStoreVerifyBlockDigest(FILE* file, size_t length, const std::string& expected,
                                      const std::atomic<bool>& cancelled) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    unsigned char buffer[4096];
    bool ok = true;
    while (length && !cancelled.load()) {
        size_t n = std::min(length, sizeof(buffer));
        if (fread(buffer, 1, n, file) != n || psa_hash_update(&op, buffer, n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        length -= n;
    }
    clearerr(file);
    if (ok && !length && !cancelled.load()) {
        return AppStoreFinishAndVerifyHash(op, expected);
    }
    psa_hash_abort(&op);
    return false;
}

static AppStoreJson AppStoreCheckpoint(const AppStoreItem& item, const std::string& etag, size_t verified) {
    AppStoreJson root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "appId", item.app_id.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "releaseId", item.id.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "size", item.size) ||
        !cJSON_AddStringToObject(root.get(), "sha256", item.sha256.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "etag", etag.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "chunkSize", 262144) ||
        !cJSON_AddNumberToObject(root.get(), "verified", verified)) {
        return AppStoreJson(nullptr, cJSON_Delete);
    }
    return root;
}

static size_t AppStoreResumePrefix(const std::string& path, const AppStoreItem& item, const std::string& etag) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return 0;
    }
    char data[1025];
    size_t bytes = fread(data, 1, sizeof(data), file);
    bool ok = !ferror(file);
    fclose(file);
    if (!ok || bytes == sizeof(data)) {
        return 0;
    }
    data[bytes] = 0;
    AppStoreJson root(cJSON_ParseWithLengthOpts(data, bytes + 1, nullptr, true), cJSON_Delete);
    std::string app, release, hash, tag;
    auto* size = cJSON_GetObjectItemCaseSensitive(root.get(), "size");
    auto* stride = cJSON_GetObjectItemCaseSensitive(root.get(), "chunkSize");
    auto* verified = cJSON_GetObjectItemCaseSensitive(root.get(), "verified");
    if (!AppStoreReadRequiredText(root.get(), "appId", app, 48) || app != item.app_id ||
        !AppStoreReadRequiredText(root.get(), "releaseId", release, 32) || release != item.id ||
        !AppStoreReadRequiredText(root.get(), "sha256", hash, 64) || hash != item.sha256 ||
        !AppStoreReadRequiredText(root.get(), "etag", tag, 96) || tag != etag || !cJSON_IsNumber(size) ||
        size->valuedouble != item.size || !cJSON_IsNumber(stride) || stride->valuedouble != 262144 ||
        !cJSON_IsNumber(verified) || verified->valuedouble < 0 || verified->valuedouble > item.size ||
        verified->valuedouble != static_cast<size_t>(verified->valuedouble)) {
        return 0;
    }
    const size_t prefix = static_cast<size_t>(verified->valuedouble);
    return prefix == item.size || prefix % 262144 == 0 ? prefix : 0;
}

static bool AppStoreSaveCheckpoint(const std::string& path, const AppStoreItem& item, const std::string& etag,
                                   size_t verified) {
    auto root = AppStoreCheckpoint(item, etag, verified);
    std::unique_ptr<char, decltype(&cJSON_free)> text(root ? cJSON_PrintUnformatted(root.get()) : nullptr, cJSON_free);
    if (!text) {
        return false;
    }
    const std::string pending = path + ".part";
    FILE* file = fopen(pending.c_str(), "wb");
    bool ok = file && fwrite(text.get(), 1, strlen(text.get()), file) == strlen(text.get());
    if (file) {
        if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
            ok = false;
        }
        if (fclose(file) != 0) {
            ok = false;
        }
    }
    if (ok) {
        ok = (SdCommitFile(pending, path, true) == SdCommitResult::Ok);
    }
    if (!ok) {
        unlink(pending.c_str());
    }
    return ok;
}

static void AppStoreClearCheckpoint(const std::string& tmp) {
    for (const char* suffix : {".json", ".json.part", ".json.cloud-backup"}) {
        if (unlink((tmp + suffix).c_str()) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "operation=install stage=checkpoint_cleanup result=failed errno=%d", errno);
        }
    }
}

static void AppStoreClearWork(const std::string& work) {
    AppStoreClearCheckpoint(work + "/download.part");
    for (const char* name : {"download.part", "manifest.json.part"}) {
        if (unlink((work + "/" + name).c_str()) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "operation=cleanup stage=work_file result=failed errno=%d", errno);
        }
    }
    if (rmdir(work.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "operation=cleanup stage=work_directory result=incomplete errno=%d", errno);
    }
}

static bool AppStoreDeleteStateFiles(const std::string& directory, bool pending_only) {
    std::unique_ptr<DIR, decltype(&closedir)> entries(opendir(directory.c_str()), closedir);
    if (!entries) {
        return errno == ENOENT;
    }
    for (;;) {
        errno = 0;
        const auto* entry = readdir(entries.get());
        if (!entry) {
            if (errno) {
                return false;
            }
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (pending_only && name != "recording.wav.part" && (!name.starts_with("data-") || !name.ends_with(".bin.part"))) {
            continue;
        }
        const auto path = directory + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) {
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return false;
        }
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            return false;
        }
    }
    entries.reset();
    return rmdir(directory.c_str()) == 0 || errno == ENOENT ||
           (pending_only && (errno == ENOTEMPTY || errno == EEXIST));
}

static bool AppStoreRecoverCheckpoint(const std::string& checkpoint) {
    struct stat st{};
    const auto backup = checkpoint + ".cloud-backup";
    if (stat(backup.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return false;
    }
    if (stat(checkpoint.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return false;
        }
        return unlink(backup.c_str()) == 0;
    }
    return errno == ENOENT && rename(backup.c_str(), checkpoint.c_str()) == 0;
}

static bool AppStoreDownload(const std::string& server, const AppStoreItem& item, const std::string& tmp,
                             std::string& error, const std::atomic<bool>& cancelled, AppStoreProgress* progress,
                             AppStoreFailure& failure) {
    if (progress) {
        progress->bytes.store(0);
    }
    auto request = AppStoreRequest(&item);
    auto manifest =
        AppStoreSendJsonRequest(server, "apps/download-manifest", request.get(), error, cancelled, false, &failure);
    if (!manifest) {
        return false;
    }
    auto* package = cJSON_GetObjectItemCaseSensitive(manifest.get(), "package");
    std::string path, chunks_path, etag, hash, release, app, version;
    auto* size = cJSON_GetObjectItemCaseSensitive(package, "size");
    size_t package_size = 0;
    auto* stride = cJSON_GetObjectItemCaseSensitive(package, "chunkSize");
    const std::string expected = server.substr(AppStoreOrigin(server).size()) + "/packages/" + item.id;
    if (!AppStoreReadRequiredText(manifest.get(), "appId", app, 48) || app != item.app_id ||
        !AppStoreReadRequiredText(manifest.get(), "version", version, 20) || version != item.version ||
        !AppStoreReadRequiredText(manifest.get(), "releaseId", release, 32) || release != item.id ||
        !AppStoreReadRequiredText(package, "path", path, 256) || path != expected ||
        !AppStoreReadRequiredText(package, "chunksPath", chunks_path, 256) || chunks_path != expected + "/chunks" ||
        !AppStoreReadRequiredText(package, "etag", etag, 96) || etag.size() < 3 || etag.front() != '"' ||
        etag.back() != '"' || etag.find_first_of("\r\n") != std::string::npos ||
        !AppStoreReadRequiredText(package, "sha256", hash, 64) || hash != item.sha256 ||
        !AppStoreParsePackageByteCount(size, package_size) || package_size != item.size || !cJSON_IsNumber(stride) ||
        stride->valuedouble != 262144) {
        ESP_LOGE(TAG, "operation=install stage=manifest result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    const std::string query = AppStoreDeviceQuery(item);
    auto chunks = AppStoreSendJsonRequest(server, ("packages/" + item.id + "/chunks" + query).c_str(), nullptr, error,
                                          cancelled, true, &failure);
    if (!chunks) {
        return false;
    }
    auto* list = cJSON_GetObjectItemCaseSensitive(chunks.get(), "chunks");
    stride = cJSON_GetObjectItemCaseSensitive(chunks.get(), "chunkSize");
    if (!AppStoreReadRequiredText(chunks.get(), "releaseId", release, 32) || release != item.id ||
        !AppStoreReadRequiredText(chunks.get(), "packageSha256", hash, 64) || hash != item.sha256 ||
        !cJSON_IsNumber(stride) || stride->valuedouble != 262144 || !cJSON_IsArray(list) ||
        cJSON_GetArraySize(list) != static_cast<int>((item.size + 262143) / 262144)) {
        ESP_LOGE(TAG, "operation=install stage=chunks result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    const std::string checkpoint = tmp + ".json";
    if (!AppStoreRecoverCheckpoint(checkpoint)) {
        ESP_LOGE(TAG, "operation=install stage=checkpoint_recover result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    const size_t prefix = AppStoreResumePrefix(checkpoint, item, etag);
    FILE* file = fopen(tmp.c_str(), "r+b");
    if (!file) {
        file = fopen(tmp.c_str(), "w+b");
    }
    if (!file) {
        ESP_LOGE(TAG, "operation=install stage=file_open result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    size_t total = 0;
    bool ok = true;
    char buffer[4096];
    cJSON* block = nullptr;
    cJSON_ArrayForEach(block, list) {
        const size_t length = std::min<size_t>(262144, item.size - total);
        auto* offset = cJSON_GetObjectItemCaseSensitive(block, "offset");
        auto* bytes = cJSON_GetObjectItemCaseSensitive(block, "length");
        auto* index = cJSON_GetObjectItemCaseSensitive(block, "index");
        std::string digest;
        size_t block_offset = 0;
        if (!AppStoreParsePackageByteCount(offset, block_offset, 0) || block_offset != total ||
            !cJSON_IsNumber(bytes) || bytes->valuedouble != length || !cJSON_IsNumber(index) ||
            index->valuedouble != total / 262144 || !AppStoreReadRequiredText(block, "sha256", digest, 64) ||
            !AppStoreIsLowercaseHex(digest, 64)) {
            ESP_LOGE(TAG, "operation=install stage=chunk_schema result=invalid");
            error = LangString_STORE_INVALID;
            ok = false;
            break;
        }
        bool complete = total + length <= prefix && fseek(file, total, SEEK_SET) == 0 &&
                        AppStoreVerifyBlockDigest(file, length, digest, cancelled);
        unsigned retry_after = 0;
        for (unsigned attempt = 0; !complete && attempt < 4 && !cancelled.load(); ++attempt) {
            if (attempt && !AppStoreWaitRetry(attempt - 1, retry_after, cancelled)) {
                break;
            }
            if (fflush(file) != 0 || ftruncate(fileno(file), total) != 0 || fseek(file, total, SEEK_SET) != 0) {
                ESP_LOGE(TAG, "operation=install stage=seek_truncate result=failed errno=%d", errno);
                error = LangString_STORE_SD_ERROR;
                break;
            }
            auto* network = Board::GetInstance().GetNetwork();
            auto http = network ? network->CreateHttp(0) : nullptr;
            if (!http) {
                ESP_LOGE(TAG, "operation=install stage=create_http result=failed");
                error = LangString_STORE_NETWORK_ERROR;
                break;
            }
            AppStoreSetRequestHeaders(http.get(), true);
            http->SetTimeout(20000);
            http->SetHeader("If-Match", etag);
            char range[96];
            snprintf(range, sizeof(range), "bytes=%zu-%zu", total, total + length - 1);
            http->SetHeader("Range", range);
            bool opened = http->Open("GET", AppStoreOrigin(server) + path + query);
            failure = AppStoreFailure{};
            failure.status = opened ? http->GetStatusCode() : -1;
            retry_after = AppStoreRetryAfter(http.get());
            char expected_range[96];
            snprintf(expected_range, sizeof(expected_range), "bytes %zu-%zu/%zu", total, total + length - 1, item.size);
            bool valid = opened && failure.status == 206 &&
                         AppStoreResponseHeader(http.get(), "content-range", "Content-Range") == expected_range &&
                         AppStoreResponseHeader(http.get(), "etag", "ETag") == etag;
            const auto encoding = AppStoreResponseHeader(http.get(), "content-encoding", "Content-Encoding");
            valid = valid && (encoding.empty() || encoding == "identity");
            psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
            if (valid && psa_hash_setup(&hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                ESP_LOGE(TAG, "operation=install stage=hash_setup result=failed");
                valid = false;
            }
            size_t received = 0;
            bool storage = false;
            while (valid && !cancelled.load()) {
                int n = http->Read(buffer, sizeof(buffer));
                if (n < 0) {
                    valid = false;
                    break;
                }
                if (!n) {
                    break;
                }
                if (static_cast<size_t>(n) > length - received) {
                    valid = false;
                    break;
                }
                if (fwrite(buffer, 1, n, file) != static_cast<size_t>(n)) {
                    storage = true;
                    break;
                }
                if (psa_hash_update(&hash_op, reinterpret_cast<const unsigned char*>(buffer), n) != PSA_SUCCESS) {
                    ESP_LOGE(TAG, "operation=install stage=hash_update result=failed");
                    valid = false;
                    break;
                }
                received += n;
            }
            if (failure.status != 206) {
                std::string body;
                if (opened && failure.status != 200) {
                    AppStoreReadBody(http.get(), body, 4096, cancelled);
                }
                if (!cancelled.load()) {
                    AppStoreParseHttpFailure(body, failure, error);
                }
                if (failure.status == 416) {
                    const std::string expected_total = "bytes */" + std::to_string(item.size);
                    ESP_LOGW(TAG, "operation=install stage=range result=invalid total_matches=%d",
                             AppStoreResponseHeader(http.get(), "content-range", "Content-Range") == expected_total);
                    AppStoreClearCheckpoint(tmp);
                }
            }
            const int transport = http->GetLastError();
            if (cancelled.load()) {
                http->Cancel();
            }
            http->Close();
            if (fflush(file) != 0) {
                storage = true;
            }
            if (storage) {
                psa_hash_abort(&hash_op);
                ESP_LOGE(TAG, "operation=install stage=write result=failed errno=%d", errno);
                error = LangString_STORE_SD_ERROR;
                break;
            }
            if (valid && received == length && !cancelled.load()) {
                complete = AppStoreFinishAndVerifyHash(hash_op, digest);
            } else {
                psa_hash_abort(&hash_op);
            }
            if (complete) {
                error.clear();
                break;
            }
            if (cancelled.load()) {
                break;
            }
            if (failure.status == 206) {
                error = LangString_STORE_VERIFY_ERROR;
            }
            ESP_LOGW(
                TAG,
                "operation=install stage=chunk result=incomplete http_status=%d transport_error=%d offset=%zu attempt=%u",
                failure.status, transport, total, attempt + 1);
            if (!failure.retryable ||
                (failure.status > 0 && failure.status != 206 && failure.status != 429 && failure.status < 500)) {
                break;
            }
        }
        if (!complete || cancelled.load()) {
            ok = false;
            break;
        }
        if (fflush(file) != 0 || fsync(fileno(file)) != 0 ||
            !AppStoreSaveCheckpoint(checkpoint, item, etag, total + length)) {
            ESP_LOGE(TAG, "operation=install stage=flush_checkpoint result=failed errno=%d", errno);
            error = LangString_STORE_SD_ERROR;
            ok = false;
            break;
        }
        total += length;
        if (progress) {
            progress->bytes.store(total);
        }
    }
    if (ok && (fflush(file) != 0 || ftruncate(fileno(file), item.size) != 0)) {
        ESP_LOGE(TAG, "operation=install stage=final_flush result=failed errno=%d", errno);
        ok = false;
        error = LangString_STORE_SD_ERROR;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "operation=install stage=file_close result=failed errno=%d", errno);
        ok = false;
        error = LangString_STORE_SD_ERROR;
    }
    if (cancelled.load()) {
        ESP_LOGI(TAG, "operation=install stage=download result=cancelled bytes=%zu", total);
    } else if (!ok && error.empty()) {
        error = LangString_STORE_NETWORK_ERROR;
    }
    return ok && !cancelled.load();
}

static bool AppStoreFindAssetSection(FILE* file, size_t length, size_t& offset, size_t& bytes) {
    AppStoreFileSource source{file, length};
    elf32_hdr_t header{};
    if (AppStoreSourceRead(&source, 0, &header, sizeof(header)) || memcmp(&header, "\177ELF\1\1", 6) ||
        header.type != 3 || header.machine != kAppStoreElfMachine ||
        header.shentsize != sizeof(elf32_shdr_t) || !header.shnum ||
        header.shnum > 512 || header.shstrndx >= header.shnum || header.shoff > length ||
        size_t(header.shnum) * sizeof(elf32_shdr_t) > length - header.shoff) {
        return false;
    }
    elf32_shdr_t names{};
    if (AppStoreSourceRead(&source, header.shoff + size_t(header.shstrndx) * sizeof(names), &names, sizeof(names))) {
        return false;
    }
    if (names.type != 3 || names.offset > length || names.size > length - names.offset) {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < header.shnum; ++i) {
        elf32_shdr_t section{};
        if (AppStoreSourceRead(&source, header.shoff + size_t(i) * sizeof(section), &section, sizeof(section))) {
            return false;
        }
        char name[15];
        if (section.name > names.size || sizeof(name) > names.size - section.name) {
            continue;
        }
        if (AppStoreSourceRead(&source, size_t(names.offset) + section.name, name, sizeof(name))) {
            return false;
        }
        if (memcmp(name, ".xz.app.assets", sizeof(name))) {
            continue;
        }
        if (found || section.type != 1 || !section.size || (section.flags & (2 | 0x800)) || section.offset > length ||
            section.size > length - section.offset) {
            return false;
        }
        offset = section.offset;
        bytes = section.size;
        found = true;
    }
    return found;
}

bool AppStoreScan(std::vector<AppStoreItem>& items, std::string& error) {
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    items.clear();
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGE(TAG, "operation=scan stage=sd_access result=failed");
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (!AppStorePrepareStorage()) {
        ESP_LOGE(TAG, "operation=scan stage=storage_layout result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    std::unique_ptr<DIR, decltype(&closedir)> entries(opendir(kAppStoreRoot.c_str()), closedir);
    if (!entries) {
        ESP_LOGE(TAG, "operation=scan stage=open_directory result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    while (auto* entry = readdir(entries.get())) {
        const std::string id = entry->d_name;
        if (!AppStoreValidInstallName(id)) {
            continue;
        }
        struct stat directory{};
        if (stat((kAppStoreRoot + "/" + id).c_str(), &directory) != 0) {
            ESP_LOGE(TAG, "operation=scan stage=entry_stat result=failed errno=%d", errno);
            error = LangString_STORE_SD_ERROR;
            return false;
        }
        if (!S_ISDIR(directory.st_mode)) {
            continue;
        }
        const std::string path = kAppStoreRoot + "/" + id + "/manifest.json";
        AppStoreItem item;
        int read_code = 0;
        const auto current = AppStoreReadManifest(path, item, read_code);
        auto read_failed = [&](AppStoreManifestStatus status) {
            if (status != AppStoreManifestStatus::IoError && status != AppStoreManifestStatus::NoMemory) {
                return false;
            }
            ESP_LOGE(TAG, "operation=scan stage=manifest result=%s app=%s errno=%d",
                     status == AppStoreManifestStatus::NoMemory ? "no_memory" : "io_error",
                     AppStoreIsValidAppId(id) ? id.c_str() : "named_app", read_code);
            error =
                status == AppStoreManifestStatus::NoMemory ? LangString_STORE_MEMORY_ERROR : LangString_STORE_SD_ERROR;
            return true;
        };
        if (read_failed(current)) {
            return false;
        }
        if (current != AppStoreManifestStatus::Ready || !AppStoreImageMatches(item, id)) {
            AppStoreItem backup;
            const auto saved = AppStoreReadManifest(path + ".cloud-backup", backup, read_code);
            if (read_failed(saved)) {
                return false;
            }
            if (saved != AppStoreManifestStatus::Ready || !AppStoreImageMatches(backup, id)) {
                if (current != AppStoreManifestStatus::Missing || saved != AppStoreManifestStatus::Missing) {
                    ESP_LOGW(TAG, "operation=scan stage=manifest_or_image result=skipped app=%s",
                             AppStoreIsValidAppId(id) ? id.c_str() : "named_app");
                }
                continue;
            }
            if (unlink(path.c_str()) != 0 && errno != ENOENT) {
                ESP_LOGE(TAG, "operation=scan stage=remove_invalid_manifest result=failed errno=%d", errno);
                continue;
            }
            if (rename((path + ".cloud-backup").c_str(), path.c_str()) != 0) {
                ESP_LOGE(TAG, "operation=scan stage=restore_manifest result=failed errno=%d", errno);
                continue;
            }
            item = std::move(backup);
            ESP_LOGW(TAG, "operation=scan stage=manifest result=restored_backup app=%s",
                     AppStoreIsValidAppId(id) ? id.c_str() : "named_app");
        } else if (unlink((path + ".cloud-backup").c_str()) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "operation=scan stage=backup_cleanup result=failed errno=%d", errno);
        }
        item.installed_name = AppStoreInstallName(item);
        AppStoreCleanVersions(AppStoreInstallDirectory(item), item.installed_name + ".elf");
        FILE* image = fopen(AppStoreImagePath(item).c_str(), "rb");
        if (image) {
            item.icon = AppStoreIconsReadElf(image, item.size);
            fclose(image);
        }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const AppStoreItem& a, const AppStoreItem& b) { return a.app_id < b.app_id; });
    ESP_LOGD(TAG, "operation=scan result=success count=%zu", items.size());
    return true;
}

bool AppStoreFetch(const std::string& server, std::vector<AppStoreItem>& items, std::string& error,
                   const std::atomic<bool>& cancelled, AppStoreCatalogPage* page) {
    AppStoreCatalogPage next = page ? *page : AppStoreCatalogPage{};
    next.expired = false;
    if (page) {
        page->expired = false;
    }
    if (!next.number || next.number > UINT32_MAX) {
        error = LangString_STORE_INVALID;
        return false;
    }
    auto request = AppStoreRequest();
    if (!request || !cJSON_AddNumberToObject(request.get(), "page", next.number) ||
        !cJSON_AddNumberToObject(request.get(), "pageSize", kAppStorePageSize) ||
        (next.number > 1 && !cJSON_AddStringToObject(request.get(), "catalogRevision", next.revision.c_str()))) {
        error = LangString_STORE_MEMORY_ERROR;
        return false;
    }
    ESP_LOGI(TAG, "operation=catalog result=start page=%zu", next.number);
    AppStoreFailure failure;
    auto root = AppStoreSendJsonRequest(server, "apps/query", request.get(), error, cancelled, false, &failure);
    auto* list = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "apps") : nullptr;
    auto* total = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "total") : nullptr;
    auto* number = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "page") : nullptr;
    auto* more = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "hasMore") : nullptr;
    auto* page_size = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "pageSize") : nullptr;
    if (!root) {
        if (page && failure.status == 409 && failure.code == "CATALOG_REVISION_EXPIRED") {
            page->expired = true;
        }
        return false;
    }
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > kAppStorePageSize || !cJSON_IsNumber(page_size) ||
        page_size->valuedouble != kAppStorePageSize || !cJSON_IsNumber(total) || total->valuedouble < 0 ||
        total->valuedouble > UINT32_MAX || total->valuedouble != static_cast<uint32_t>(total->valuedouble) ||
        !cJSON_IsNumber(number) || number->valuedouble != next.number || !cJSON_IsBool(more) ||
        !AppStoreReadRequiredText(root.get(), "catalogRevision", next.revision, 128) || next.revision.empty() ||
        (cJSON_IsTrue(more) && cJSON_GetArraySize(list) == 0)) {
        ESP_LOGE(TAG, "operation=catalog stage=schema result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    std::vector<AppStoreItem> result;
    cJSON* value = nullptr;
    cJSON_ArrayForEach(value, list) {
        AppStoreItem item;
        if (!AppStoreParseCatalogItem(value, item) ||
            std::any_of(result.begin(), result.end(),
                        [&](const AppStoreItem& old) { return old.app_id == item.app_id; })) {
            ESP_LOGE(TAG, "operation=catalog stage=item result=invalid");
            error = LangString_STORE_INVALID;
            return false;
        }
        result.push_back(std::move(item));
    }
    if (page && page->number > 1 && next.revision != page->revision) {
        ESP_LOGE(TAG, "operation=catalog stage=revision result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    next.total = static_cast<size_t>(total->valuedouble);
    next.more = cJSON_IsTrue(more);
    const uint64_t end = (static_cast<uint64_t>(next.number) - 1) * kAppStorePageSize + result.size();
    if ((next.more && end >= next.total) || (!next.more && end < next.total)) {
        ESP_LOGE(TAG, "operation=catalog stage=pagination result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    items = std::move(result);
    if (page) {
        *page = std::move(next);
    }
    ESP_LOGI(TAG, "operation=catalog result=success count=%zu", items.size());
    return true;
}

bool AppStoreCheckUpdate(const std::string& server, const AppStoreItem& current, AppStoreItem& candidate,
                         bool& available, std::string& error, const std::atomic<bool>& cancelled) {
    auto request = AppStoreRequest(&current);
    if (request) {
        cJSON_DeleteItemFromObjectCaseSensitive(request.get(), "releaseId");
    }
    if (!request || !cJSON_AddStringToObject(request.get(), "currentVersion", current.version.c_str())) {
        error = LangString_STORE_MEMORY_ERROR;
        return false;
    }
    auto root = AppStoreSendJsonRequest(server, "apps/check-update", request.get(), error, cancelled);
    if (!root) {
        return false;
    }
    auto* update = cJSON_GetObjectItemCaseSensitive(root.get(), "hasUpdate");
    auto* release = cJSON_GetObjectItemCaseSensitive(root.get(), "release");
    std::string app;
    unsigned parts[3];
    if (!AppStoreReadRequiredText(root.get(), "appId", app, 48) || app != current.app_id || !cJSON_IsBool(update) ||
        (!cJSON_IsTrue(update) && !cJSON_IsNull(release))) {
        ESP_LOGE(TAG, "operation=check_update stage=schema result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    available = false;
    if (cJSON_IsTrue(update)) {
        AppStoreItem result = current;
        auto* size = cJSON_GetObjectItemCaseSensitive(release, "size");
        if (!AppStoreReadRequiredText(release, "releaseId", result.id, 32) || !AppStoreIsLowercaseHex(result.id, 32) ||
            !AppStoreReadRequiredText(release, "version", result.version, 20) ||
            !AppStoreParseVersion(result.version, parts) || !AppStoreNewer(result.version, current.version) ||
            !AppStoreReadRequiredText(release, "sha256", result.sha256, 64) ||
            !AppStoreIsLowercaseHex(result.sha256, 64) || !AppStoreParsePackageByteCount(size, result.size) ||
            !AppStoreReadRequiredText(release, "releaseNotes", result.release_notes, 1024) ||
            !AppStoreReadRequiredText(release, "releaseNotesEn", result.release_notes_en, 1024)) {
            ESP_LOGE(TAG, "operation=check_update stage=release result=invalid");
            error = LangString_STORE_INVALID;
            return false;
        }
        candidate = std::move(result);
        available = true;
    }
    return true;
}

bool AppStoreDetail(const std::string& server, AppStoreItem& item, std::string& error,
                    const std::atomic<bool>& cancelled) {
    auto request = AppStoreRequest(&item);
    auto root = AppStoreSendJsonRequest(server, "apps/detail", request.get(), error, cancelled);
    if (!root) {
        return false;
    }
    AppStoreItem result;
    if (!AppStoreParseCatalogItem(cJSON_GetObjectItemCaseSensitive(root.get(), "app"), result) ||
        result.app_id != item.app_id || result.id != item.id || result.sha256 != item.sha256 ||
        result.size != item.size || result.version != item.version) {
        ESP_LOGE(TAG, "operation=detail stage=item result=invalid");
        error = LangString_STORE_INVALID;
        return false;
    }
    result.installed_name = item.installed_name;
    item = std::move(result);
    return true;
}

bool AppStoreInstall(const std::string& server, const AppStoreItem& requested, std::string& error,
                     const std::atomic<bool>& cancelled, AppStoreProgress* progress) {
    AppStoreItem item = requested;
    if (cancelled.load()) {
        ESP_LOGI(TAG, "operation=install result=cancelled");
        error = LangString_STORE_CANCELLED;
        return false;
    }
    const int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "operation=install result=start app=%s bytes=%zu",
             AppStoreIsValidAppId(item.app_id) ? item.app_id.c_str() : "invalid", item.size);
    if (progress) {
        progress->total.store(item.size);
    }
    if (!AppStoreNetworkReady(error, cancelled)) {
        return false;
    }
    if (!AppStoreDetail(server, item, error, cancelled)) {
        return false;
    }
    std::vector<AppStoreItem> installed;
    if (!AppStoreScan(installed, error)) {
        return false;
    }
    auto old = std::find_if(installed.begin(), installed.end(),
                            [&](const AppStoreItem& v) { return v.app_id == item.app_id; });
    if (old != installed.end() && !AppStoreNewer(item.version, old->version)) {
        ESP_LOGI(TAG, "operation=install result=already_current");
        error = LangString_STORE_CURRENT;
        return false;
    }
    SdAccessTransaction transaction;
    item.installed_name = old == installed.end() ? AppStoreInstallName(item) : AppStoreInstallName(*old);
    if (std::any_of(installed.begin(), installed.end(), [&](const AppStoreItem& other) {
            return other.app_id != item.app_id &&
                   strcasecmp(AppStoreInstallName(other).c_str(), item.installed_name.c_str()) == 0;
        })) {
        ESP_LOGE(TAG, "operation=install stage=name_conflict result=failed");
        error = LangString_STORE_INVALID;
        return false;
    }
    const std::string dir = AppStoreInstallDirectory(item);
    if (!transaction) {
        ESP_LOGE(TAG, "operation=install stage=sd_access result=failed");
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (!SdEnsureDir(dir.c_str())) {
        ESP_LOGE(TAG, "operation=install stage=directory result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    const auto work = AppStoreWorkDirectory(item);
    if (work.empty() || !SdEnsureDir(work.c_str())) {
        ESP_LOGE(TAG, "operation=install stage=work_directory result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    const std::string tmp = work + "/download.part", image = AppStoreImagePath(item);
    AppStoreFailure failure;
    bool downloaded = false;
    for (unsigned attempt = 0; attempt < 4 && !cancelled.load(); ++attempt) {
        error.clear();
        failure = AppStoreFailure{};
        downloaded = AppStoreDownload(server, item, tmp, error, cancelled, progress, failure);
        if (downloaded || failure.status != 412 || attempt == 3) {
            break;
        }
        ESP_LOGW(TAG, "operation=install stage=manifest result=refresh attempt=%u", attempt + 1);
    }
    if (!downloaded) {
        if (cancelled.load()) {
            error = LangString_STORE_CANCELLED;
        }
        return false;
    }
    FILE* f = nullptr;
    bool ok = false;
    if (progress) {
        progress->verifying.store(true);
    }
    f = fopen(tmp.c_str(), "rb");
    ok = f && AppStoreVerifyFileDigest(f, item.size, item.sha256, &cancelled) && AppStoreValidateImage(f, item.size);
    if (f) {
        fclose(f);
    }
    if (!ok) {
        if (cancelled.load()) {
            ESP_LOGI(TAG, "operation=install stage=verify result=cancelled");
            error = LangString_STORE_CANCELLED;
        } else {
            ESP_LOGE(TAG, "operation=install stage=verify result=failed");
            AppStoreClearWork(work);
            error = LangString_STORE_VERIFY_ERROR;
        }
        return false;
    }
    ESP_LOGD(TAG, "operation=install stage=verify result=success bytes=%zu", item.size);
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    const bool changed_image = old == installed.end() || old->sha256 != item.sha256;
    const std::string previous = old == installed.end() ? std::string{} : dir + "/" + old->sha256 + ".elf";
    bool saved_previous = false;
    auto rollback_image = [&]() {
        if (!changed_image) {
            return;
        }
        // 未提交的新包移回下载目录，保留下次安装可复用的数据。
        if (rename(image.c_str(), tmp.c_str()) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "operation=install stage=rollback_download result=failed errno=%d", errno);
        }
        if (saved_previous && rename(previous.c_str(), image.c_str()) != 0) {
            ESP_LOGE(TAG, "operation=install stage=rollback_image result=failed errno=%d", errno);
        }
    };
    auto stop_cancelled = [&]() {
        if (!cancelled.load()) {
            return false;
        }
        ESP_LOGI(TAG, "operation=install result=cancelled bytes=%zu", item.size);
        error = LangString_STORE_CANCELLED;
        return true;
    };
    if (stop_cancelled()) {
        return false;
    }
    f = fopen(image.c_str(), "rb");
    const bool reusable = f && AppStoreVerifyFileDigest(f, item.size, item.sha256, &cancelled);
    if (f) {
        fclose(f);
    }
    if (stop_cancelled()) {
        return false;
    }
    if (reusable) {
        unlink(tmp.c_str());
    } else {
        struct stat st{};
        if (stat(image.c_str(), &st) == 0) {
            if (old == installed.end() || stat(previous.c_str(), &st) == 0 || errno != ENOENT ||
                rename(image.c_str(), previous.c_str()) != 0) {
                ESP_LOGE(TAG, "operation=install stage=image_backup result=failed errno=%d", errno);
                error = LangString_STORE_SD_ERROR;
                return false;
            }
            saved_previous = true;
        } else if (errno != ENOENT) {
            ESP_LOGE(TAG, "operation=install stage=image_stat result=failed errno=%d", errno);
            error = LangString_STORE_SD_ERROR;
            return false;
        }
        if (rename(tmp.c_str(), image.c_str()) != 0) {
            ESP_LOGE(TAG, "operation=install stage=image_commit result=failed errno=%d", errno);
            if (saved_previous && rename(previous.c_str(), image.c_str()) != 0) {
                ESP_LOGE(TAG, "operation=install stage=rollback_image result=failed errno=%d", errno);
            }
            error = LangString_STORE_SD_ERROR;
            return false;
        }
    }
    auto json = AppStoreSerializeManifestItem(item);
    std::unique_ptr<char, decltype(&cJSON_free)> text(json ? cJSON_PrintUnformatted(json.get()) : nullptr, cJSON_free);
    if (!text) {
        ESP_LOGE(TAG, "operation=install stage=manifest_serialize result=no_memory");
        rollback_image();
        error = LangString_STORE_MEMORY_ERROR;
        return false;
    }
    const std::string manifest = dir + "/manifest.json", pending = work + "/manifest.json.part";
    f = fopen(pending.c_str(), "wb");
    ok = f && fwrite(text.get(), 1, strlen(text.get()), f) == strlen(text.get());
    if (f) {
        if (fflush(f) != 0) {
            ok = false;
        }
        if (fclose(f) != 0) {
            ok = false;
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "operation=install stage=manifest_write result=failed errno=%d", errno);
        unlink(pending.c_str());
        rollback_image();
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (stop_cancelled()) {
        unlink(pending.c_str());
        rollback_image();
        return false;
    }
    // 清单切换是安装提交点；提交后完成清理，不再回滚已安装版本。
    if (!(SdCommitFile(pending, manifest, true) == SdCommitResult::Ok)) {
        ESP_LOGE(TAG, "operation=install stage=manifest_commit result=failed errno=%d", errno);
        unlink(pending.c_str());
        rollback_image();
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    AppStoreClearWork(work);
    AppStoreCleanVersions(dir, item.installed_name + ".elf");
    ESP_LOGI(TAG, "operation=install result=success bytes=%zu elapsed_ms=%lld", item.size,
             (long long)((esp_timer_get_time() - started) / 1000));
    return true;
}

bool AppStoreRemove(const AppStoreItem& item, std::string& error) {
    ESP_LOGI(TAG, "operation=remove result=start");
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGE(TAG, "operation=remove stage=sd_access result=failed");
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (!AppStoreIsValidAppId(item.app_id) || !AppStoreValidInstallName(AppStoreInstallName(item))) {
        error = LangString_STORE_INVALID;
        return false;
    }
    const auto storage_key = AppStoreInstallName(item) + "--" + item.app_id;
    const auto state = std::string(SD_PATH_APP_STATE) + "/" + storage_key;
    const auto state_work = std::string(SD_PATH_APP_STORE_TEMP) + "/" + storage_key;
    const auto work = AppStoreWorkDirectory(item);
    // 状态删除失败时保留安装清单，使用户可以重试卸载。
    if (!AppStoreDeleteStateFiles(state, false) || !AppStoreDeleteStateFiles(state_work, true) ||
        (!work.empty() && work != state_work && !AppStoreDeleteStateFiles(work, true))) {
        ESP_LOGE(TAG, "operation=remove stage=state_delete result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    const std::string dir = AppStoreInstallDirectory(item), manifest = dir + "/manifest.json";
    if (unlink((manifest + ".cloud-backup").c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "operation=remove stage=backup_delete result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (unlink(manifest.c_str()) != 0) {
        ESP_LOGE(TAG, "operation=remove stage=manifest_delete result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    if (unlink(AppStoreImagePath(item).c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "operation=remove stage=image_delete result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    AppStoreCleanVersions(dir, "");
    if (!work.empty()) {
        AppStoreClearWork(work);
    }
    if (rmdir(dir.c_str()) != 0) {
        ESP_LOGW(TAG, "operation=remove stage=directory_cleanup result=incomplete errno=%d", errno);
    }
    ESP_LOGI(TAG, "operation=remove result=success");
    return true;
}

bool AppStoreLoad(const AppStoreItem& item, esp_elf_t& elf, std::string& error) {
    ESP_LOGI(TAG, "operation=load result=start app=%s bytes=%zu",
             AppStoreIsValidAppId(item.app_id) ? item.app_id.c_str() : "invalid", item.size);
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    static bool symbols_ready = false;
    if (!symbols_ready) {
        const int result = esp_elf_register_symbol(kAppStoreRuntimeMath);
        if (result != 0 && result != -EEXIST) {
            ESP_LOGE(TAG, "operation=load stage=register_symbols result=failed code=%d", result);
            error = LangString_STORE_RUN_ERROR;
            return false;
        }
        symbols_ready = true;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGE(TAG, "operation=load stage=sd_access result=failed");
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    FILE* file = fopen(AppStoreImagePath(item).c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "operation=load stage=file_open result=failed errno=%d", errno);
        error = LangString_STORE_SD_ERROR;
        return false;
    }
    const bool verified = AppStoreVerifyFileDigest(file, item.size, item.sha256);
    bool ok = verified;
    int relocate_error = 0;
    if (ok) {
        AppStoreFileSource context{file, item.size};
        const elf_source_t source{AppStoreSourceSize, AppStoreSourceRead, nullptr, &context};
        relocate_error = esp_elf_relocate_source(&elf, &source);
        ok = relocate_error == 0;
    }
    fclose(file);
    if (!ok) {
        ESP_LOGE(TAG, "operation=load stage=%s result=failed code=%d", verified ? "relocate" : "verify",
                 relocate_error);
        error = verified ? LangString_STORE_RUN_ERROR : LangString_STORE_VERIFY_ERROR;
    } else {
        ESP_LOGI(TAG, "operation=load result=success");
    }
    return ok;
}

size_t AppStoreReadAsset(const AppStoreItem& item, size_t start, void* buffer, size_t length) {
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    SdAccessTransaction transaction;
    if (!buffer || !length) {
        return 0;
    }
    if (!transaction) {
        ESP_LOGE(TAG, "operation=asset_read stage=sd_access result=failed");
        return 0;
    }
    FILE* file = fopen(AppStoreImagePath(item).c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "operation=asset_read stage=file_open result=failed errno=%d", errno);
        return 0;
    }
    size_t offset = 0, bytes = 0;
    const bool ok = AppStoreFindAssetSection(file, item.size, offset, bytes) && start <= bytes &&
                    length <= bytes - start && fseek(file, offset + start, SEEK_SET) == 0 &&
                    fread(buffer, 1, length, file) == length;
    fclose(file);
    if (!ok) {
        ESP_LOGE(TAG, "operation=asset_read stage=read result=failed offset=%zu bytes=%zu", start, length);
    }
    return ok ? length : 0;
}

size_t AppStoreAssetSize(const AppStoreItem& item) {
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGE(TAG, "operation=asset_size stage=sd_access result=failed");
        return 0;
    }
    FILE* file = fopen(AppStoreImagePath(item).c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "operation=asset_size stage=file_open result=failed errno=%d", errno);
        return 0;
    }
    size_t offset = 0, bytes = 0;
    if (!AppStoreFindAssetSection(file, item.size, offset, bytes)) {
        bytes = 0;
    }
    fclose(file);
    return bytes;
}

bool AppStoreLocateAsset(const AppStoreItem& item, std::string& path, size_t& offset, size_t& length) {
    std::lock_guard<std::mutex> lock(app_store_s_storage);
    SdAccessTransaction transaction;
    if (!transaction) {
        return false;
    }
    path = AppStoreImagePath(item);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "operation=asset_locate stage=open result=failed errno=%d", errno);
        return false;
    }
    const bool valid = AppStoreFindAssetSection(file, item.size, offset, length);
    fclose(file);
    if (!valid) {
        ESP_LOGE(TAG, "operation=asset_locate stage=section result=invalid");
    }
    return valid;
}

bool AppStoreNewer(const std::string& candidate, const std::string& current) {
    unsigned a[3], b[3];
    if (!AppStoreParseVersion(candidate, a) || !AppStoreParseVersion(current, b)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

bool AppStoreNormalizeServer(std::string& server) {
    while (!server.empty() && server.back() == '/') {
        server.pop_back();
    }
    if (server.size() > 200 || (server.compare(0, 7, "http://") && server.compare(0, 8, "https://"))) {
        return false;
    }
    const size_t begin = server.find("://") + 3;
    const size_t slash = server.find('/', begin);
    const std::string authority = server.substr(begin, slash == std::string::npos ? slash : slash - begin);
    if (authority.empty() || server.find_first_of("\\?#@ \r\n\t", begin) != std::string::npos) {
        return false;
    }
    if (slash == std::string::npos) {
        server += "/xiaozhi/api/v1/device/app-store";
    } else if (server.substr(slash) != "/xiaozhi/api/v1/device/app-store" &&
               server.substr(slash) != "/api/v1/device/app-store") {
        return false;
    }
    return true;
}

const char* AppStoreName(const AppStoreItem& item) {
    return strcmp(LangCurrent(), "zh-CN") == 0 ? item.name.c_str() : item.name_en.c_str();
}
