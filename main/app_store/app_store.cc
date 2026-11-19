#include "app_store.h"
#include "app_store_icons.h"
#include "private/elf_validate.h"
#include <mbedtls/base64.h>
#include "assets/lang_app_store.h"
#include "assets/lang_config.h"
#include "api_http.h"
#include "board.h"
#include "system_info.h"
#include "app_api.h"
#include <esp_app_desc.h>
#include <esp_random.h>
#include "power_policy.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "reader/http_download_file.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <psa/crypto.h>
#include <esp_log.h>
#include <esp_timer.h>

extern "C" {
#define XZ_APP_RUNTIME_MATH(name, result, args) result name args;
#include "app_runtime_math.def"
#undef XZ_APP_RUNTIME_MATH
}

namespace AppStore {
namespace {
const esp_elf_symbol_table_t kRuntimeMath[] = {
#define XZ_APP_RUNTIME_MATH(name, result, args) ESP_ELFSYM_EXPORT(name),
#include "app_runtime_math.def"
#undef XZ_APP_RUNTIME_MATH
    ESP_ELFSYM_END
};
constexpr const char* TAG = "AppStore";
constexpr size_t kMaxImage = 32 * 1024 * 1024;
constexpr size_t kPageSize = 16;
const std::string kRoot = SD_PATH_APPS;
std::mutex s_storage;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool Identifier(const std::string& s) {
    if (s.empty() || s.size() > 48 || s[0] < 'a' || s[0] > 'z') return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool Hex(const std::string& s, size_t length) {
    return s.size() == length && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Version(const std::string& s, unsigned parts[3]) {
    if (s.size() > 20) return false;
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t begin = pos;
        parts[i] = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (pos - begin >= 6) return false;
            parts[i] = parts[i] * 10 + s[pos++] - '0';
        }
        if (pos == begin || (pos - begin > 1 && s[begin] == '0')) return false;
        if (i < 2 && (pos >= s.size() || s[pos++] != '.')) return false;
    }
    return pos == s.size();
}
bool Text(cJSON* root, const char* key, std::string& out, size_t max) {
    auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) > max) return false;
    out = v->valuestring;
    return true;
}
bool OptionalText(cJSON* root, const char* key, std::string& out, size_t max) {
    if (!cJSON_GetObjectItemCaseSensitive(root, key)) { out.clear(); return true; }
    return Text(root, key, out, max);
}
static bool ValidInstallName(const std::string& name) {
    return !name.empty() && name.size() <= 72 && name.front() != '.' && SdIsValidFileName(name.c_str());
}
static std::string InstallName(const Item& item) {
    if (!item.installed_name.empty()) return item.installed_name;
    std::string name = item.name.empty() ? item.name_en : item.name;
    name.resize(SdUtf8PrefixLength(name.c_str(), 72));
    for (char& c : name) if (static_cast<unsigned char>(c) < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "app";
    if (name.front() == '.') name.front() = '_';
    return name;
}
static std::string InstallDirectory(const Item& item) { return kRoot + "/" + InstallName(item); }
static std::string ImagePath(const Item& item) { return InstallDirectory(item) + "/" + InstallName(item) + ".elf"; }
bool Parse(cJSON* root, Item& item) {
    std::string target;
    unsigned parts[3];
    auto* abi = cJSON_GetObjectItemCaseSensitive(root, "abiVersion");
    auto* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!Text(root,"id",item.id,32) || !Hex(item.id,32) ||
        !Text(root,"appId",item.app_id,48) || !Identifier(item.app_id) ||
        !Text(root,"name",item.name,128) || item.name.empty() ||
        !Text(root,"nameEn",item.name_en,192) || item.name_en.empty() ||
        !Text(root,"description",item.description,1024) ||
        !Text(root,"descriptionEn",item.description_en,1024) ||
        !OptionalText(root,"installedName",item.installed_name,72) ||
        (!item.installed_name.empty() && !ValidInstallName(item.installed_name)) ||
        !OptionalText(root,"summary",item.summary,256) ||
        !OptionalText(root,"summaryEn",item.summary_en,256) ||
        !OptionalText(root,"category",item.category,96) ||
        !OptionalText(root,"categoryEn",item.category_en,96) ||
        !OptionalText(root,"languages",item.languages,263) ||
        !OptionalText(root,"provider",item.provider,128) ||
        !OptionalText(root,"releaseNotes",item.release_notes,1024) ||
        !OptionalText(root,"releaseNotesEn",item.release_notes_en,1024) ||
        !OptionalText(root,"uploadedAt",item.uploaded_at,64) ||
        !Text(root,"version",item.version,20) || !Version(item.version,parts) ||
        !Text(root,"sha256",item.sha256,64) || !Hex(item.sha256,64) ||
        !Text(root,"target",target,32) || target != "esp32s31" ||
        !cJSON_IsNumber(abi) || abi->valuedouble != 1 ||
        !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > kMaxImage ||
        size->valuedouble != static_cast<size_t>(size->valuedouble)) return false;
    item.size = static_cast<size_t>(size->valuedouble);
    auto* icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    if (icon) {
        if (!cJSON_IsString(icon) || !icon->valuestring || strlen(icon->valuestring) != 2752) return false;
        unsigned char data[2064];
        size_t length = 0;
        if (mbedtls_base64_decode(data, sizeof(data), &length,
                reinterpret_cast<const unsigned char*>(icon->valuestring), 2752) != 0) return false;
        item.icon = AppStoreIcons::Decode(data, length);
        if (!item.icon) return false;
    }
    return true;
}
Json Serialize(const Item& item) {
    Json j(cJSON_CreateObject(), cJSON_Delete);
    if (!j) return j;
    if (!cJSON_AddStringToObject(j.get(),"id",item.id.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"appId",item.app_id.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"installedName",InstallName(item).c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"name",item.name.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"nameEn",item.name_en.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"description",item.description.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"descriptionEn",item.description_en.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"summary",item.summary.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"summaryEn",item.summary_en.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"category",item.category.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"categoryEn",item.category_en.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"languages",item.languages.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"provider",item.provider.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"releaseNotes",item.release_notes.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"releaseNotesEn",item.release_notes_en.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"uploadedAt",item.uploaded_at.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"version",item.version.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"sha256",item.sha256.c_str())) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddStringToObject(j.get(),"target","esp32s31")) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddNumberToObject(j.get(),"abiVersion",1)) return Json(nullptr,cJSON_Delete);
    if (!cJSON_AddNumberToObject(j.get(),"size",item.size)) return Json(nullptr,cJSON_Delete);
    return j;
}
bool NetworkReady(std::string& error, const std::atomic<bool>& cancelled) {
    auto& board = Board::GetInstance();
    const bool ready = !board.GetNetworkErrorText(nullptr) && board.EnsureNetworkReady(20000);
    if (cancelled.load()) {
        ESP_LOGI(TAG,"operation=network result=cancelled");
        error = Lang::Strings::STORE_CANCELLED;
        return false;
    }
    if (ready) return true;
    ESP_LOGW(TAG, "operation=network stage=ready result=unavailable timeout_ms=20000");
    error = Lang::Strings::STORE_NETWORK_ERROR;
    return false;
}
bool Directory(const std::string& path) {
    if (mkdir(path.c_str(),0755) == 0) return true;
    struct stat st {};
    return errno == EEXIST && stat(path.c_str(),&st) == 0 && S_ISDIR(st.st_mode);
}

static bool PrepareStorage() {
    return Directory(SD_MOUNT_POINT "/metalio") && Directory(SD_APP_ROOT) &&
        Directory(kRoot) && Directory(SD_PATH_TEMP) && Directory(SD_PATH_APP_STORE_TEMP);
}

static std::string WorkDirectory(const Item& item) {
    if (!Identifier(item.app_id)) { errno = EINVAL; return {}; }
    // 名称只作可读标签，稳定 ID 避免重名；改名后仍按 ID 查找续传数据。
    const std::string suffix = "--" + item.app_id;
    DIR* entries = opendir(SD_PATH_APP_STORE_TEMP);
    if (!entries) return {};
    std::string found;
    while (auto* entry = readdir(entries)) {
        const std::string name = entry->d_name;
        if (SdIsValidFileName(name.c_str()) && name.size() > suffix.size() && name.ends_with(suffix)) {
            found = std::string(SD_PATH_APP_STORE_TEMP) + "/" + name;
            struct stat st {};
            if (stat(found.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { found.clear(); continue; }
            break;
        }
    }
    closedir(entries);
    return found.empty() ? std::string(SD_PATH_APP_STORE_TEMP) + "/" + InstallName(item) + suffix : found;
}

enum class ManifestStatus {
    Missing, // 尚无安装清单
    Invalid, // 清单内容无效
    IoError, // 清单读写失败
    NoMemory, // 无法分配读取缓冲
    Ready // 清单读取成功
};
ManifestStatus ReadManifest(const std::string& path, Item& item, int& code) {
    code=0;
    FILE* f = fopen(path.c_str(),"rb");
    if (!f) { code=errno; return code == ENOENT ? ManifestStatus::Missing : ManifestStatus::IoError; }
    std::unique_ptr<char[]> data(new(std::nothrow) char[16385]);
    if (!data) { code=ENOMEM; fclose(f); return ManifestStatus::NoMemory; }
    const size_t n = fread(data.get(),1,16385,f);
    bool ok = !ferror(f);
    if (!ok) code=errno;
    if (fclose(f)!=0 && ok) { code=errno; ok=false; }
    if (!ok) return ManifestStatus::IoError;
    if (n >= 16385) return ManifestStatus::Invalid;
    data[n] = 0;
    Json j(cJSON_ParseWithLengthOpts(data.get(),n+1,nullptr,true),cJSON_Delete);
    return j && Parse(j.get(),item) ? ManifestStatus::Ready : ManifestStatus::Invalid;
}
bool FinishHash(psa_hash_operation_t& op,const std::string& expected) {
    unsigned char hash[32];size_t length=0;
    const bool ok=psa_hash_finish(&op,hash,sizeof(hash),&length)==PSA_SUCCESS&&length==sizeof(hash);
    psa_hash_abort(&op);if(!ok)return false;
    char hex[65];for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
    return expected==hex;
}
bool Digest(FILE* f, size_t expected_size, const std::string& expected_hash, const std::atomic<bool>* cancelled = nullptr) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op,PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    unsigned char buf[4096];
    size_t total = 0;
    bool ok = true;
    for (;;) {
        if (cancelled && cancelled->load()) { ok=false; break; }
        size_t n = fread(buf,1,sizeof(buf),f);
        if (!n) { ok = !ferror(f); break; }
        if (total > expected_size || n > expected_size-total) { ok = false; break; }
        total += n;
        if (psa_hash_update(&op,buf,n) != PSA_SUCCESS) { ok = false; break; }
    }
    if(ok&&total==expected_size)return FinishHash(op,expected_hash);
    psa_hash_abort(&op);return false;
}
static bool ImageMatches(const Item& item, const std::string& directory) {
    if (directory != InstallName(item)) return false;
    const auto dir = kRoot + "/" + directory;
    const auto image = dir + "/" + InstallName(item) + ".elf";
    const auto saved = dir + "/" + item.sha256 + ".elf";
    struct stat st {};
    const bool have_saved = stat(saved.c_str(), &st) == 0;
    if (stat(image.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == item.size) {
        if (!have_saved) return true;
        FILE* file = fopen(image.c_str(), "rb");
        const bool valid = file && Digest(file, item.size, item.sha256);
        if (file) fclose(file);
        if (valid) return true;
    }
    FILE* file = fopen(saved.c_str(), "rb");
    const bool valid = file && Digest(file, item.size, item.sha256);
    if (file) fclose(file);
    if (!valid) return false;
    if ((unlink(image.c_str()) != 0 && errno != ENOENT) || rename(saved.c_str(), image.c_str()) != 0) {
        ESP_LOGE(TAG, "operation=scan stage=image_recover result=failed errno=%d", errno);
        return false;
    }
    return true;
}
void CleanVersions(const std::string& dir, const std::string& keep) {
    DIR* d = opendir(dir.c_str());
    if (!d) { ESP_LOGW(TAG, "operation=cleanup stage=open_directory result=failed errno=%d",errno); return; }
    while (auto* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size()==68 && name.substr(64)==".elf" && Hex(name.substr(0,64),64) && name != keep && unlink((dir+"/"+name).c_str())!=0) ESP_LOGW(TAG, "operation=cleanup stage=unlink result=failed errno=%d",errno);
    }
    closedir(d);
}
}

const char* Name(const Item& item) { return strcmp(Lang::Current(),"zh-CN")==0 ? item.name.c_str() : item.name_en.c_str(); }
bool Newer(const std::string& candidate,const std::string& current) {
    unsigned a[3],b[3];
    if (!Version(candidate,a) || !Version(current,b)) return false;
    for (int i=0;i<3;++i) if(a[i]!=b[i]) return a[i]>b[i];
    return false;
}
bool NormalizeServer(std::string& server) {
    while (!server.empty() && server.back()=='/') server.pop_back();
    if (server.size()>200 || (server.compare(0,7,"http://") && server.compare(0,8,"https://"))) return false;
    const size_t begin = server.find("://")+3;
    const size_t slash = server.find('/',begin);
    const std::string authority=server.substr(begin,slash==std::string::npos ? slash:slash-begin);
    if(authority.empty() || server.find_first_of("\\?#@ \r\n\t",begin)!=std::string::npos) return false;
    if(slash==std::string::npos) server+="/xiaozhi/api/v1/device/app-store";
    else if(server.substr(slash)!="/xiaozhi/api/v1/device/app-store" && server.substr(slash)!="/api/v1/device/app-store") return false;
    return true;
}
bool Scan(std::vector<Item>& items,std::string& error) {
    std::lock_guard<std::mutex> lock(s_storage);
    items.clear();
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGE(TAG, "operation=scan stage=sd_access result=failed"); error=Lang::Strings::STORE_SD_ERROR; return false; }
    if (!PrepareStorage()) { ESP_LOGE(TAG, "operation=scan stage=storage_layout result=failed errno=%d",errno); error=Lang::Strings::STORE_SD_ERROR; return false; }
    std::unique_ptr<DIR, decltype(&closedir)> entries(opendir(kRoot.c_str()), closedir);
    if (!entries) { ESP_LOGE(TAG, "operation=scan stage=open_directory result=failed errno=%d",errno); error=Lang::Strings::STORE_SD_ERROR; return false; }
    while (auto* entry = readdir(entries.get())) {
        const std::string id = entry->d_name;
        if (!ValidInstallName(id)) continue;
        struct stat directory {};
        if (stat((kRoot+"/"+id).c_str(),&directory)!=0) {
            ESP_LOGE(TAG,"operation=scan stage=entry_stat result=failed errno=%d",errno);
            error=Lang::Strings::STORE_SD_ERROR; return false;
        }
        if (!S_ISDIR(directory.st_mode)) continue;
        const std::string path=kRoot+"/"+id+"/manifest.json";
        Item item;
        int read_code=0;
        const auto current=ReadManifest(path,item,read_code);
        auto read_failed=[&](ManifestStatus status) {
            if (status!=ManifestStatus::IoError && status!=ManifestStatus::NoMemory) return false;
            ESP_LOGE(TAG,"operation=scan stage=manifest result=%s app=%s errno=%d",status==ManifestStatus::NoMemory ? "no_memory" : "io_error",Identifier(id) ? id.c_str() : "named_app",read_code);
            error=status==ManifestStatus::NoMemory ? Lang::Strings::STORE_MEMORY_ERROR : Lang::Strings::STORE_SD_ERROR;
            return true;
        };
        if (read_failed(current)) { return false; }
        if (current!=ManifestStatus::Ready || !ImageMatches(item,id)) {
            Item backup;
            const auto saved=ReadManifest(path+".cloud-backup",backup,read_code);
            if (read_failed(saved)) { return false; }
            if (saved!=ManifestStatus::Ready || !ImageMatches(backup,id)) {
                if (current!=ManifestStatus::Missing || saved!=ManifestStatus::Missing)
                    ESP_LOGW(TAG,"operation=scan stage=manifest_or_image result=skipped app=%s",Identifier(id) ? id.c_str() : "named_app");
                continue;
            }
            if (unlink(path.c_str())!=0 && errno!=ENOENT) { ESP_LOGE(TAG,"operation=scan stage=remove_invalid_manifest result=failed errno=%d",errno); continue; }
            if(rename((path+".cloud-backup").c_str(),path.c_str())!=0) { ESP_LOGE(TAG, "operation=scan stage=restore_manifest result=failed errno=%d",errno); continue; }
            item=std::move(backup);
            ESP_LOGW(TAG, "operation=scan stage=manifest result=restored_backup app=%s",Identifier(id) ? id.c_str() : "named_app");
        } else if (unlink((path+".cloud-backup").c_str())!=0 && errno!=ENOENT) {
            ESP_LOGW(TAG,"operation=scan stage=backup_cleanup result=failed errno=%d",errno);
        }
        item.installed_name = InstallName(item);
        CleanVersions(InstallDirectory(item), item.installed_name+".elf");
        FILE* image = fopen(ImagePath(item).c_str(), "rb");
        if (image) { item.icon = AppStoreIcons::ReadElf(image, item.size); fclose(image); }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(),items.end(),[](const Item& a,const Item& b){ return a.app_id<b.app_id; });
    ESP_LOGD(TAG, "operation=scan result=success count=%zu",items.size());
    return true;
}
namespace {
std::string Origin(const std::string& server) {
    return server.substr(0,server.find('/',server.find("://")+3));
}
struct Failure {
    int status = -1; // HTTP 状态，负值表示传输失败
    unsigned retry_after = 0; // 服务端建议等待秒数
    bool retryable = true; // 是否允许有限重试
    std::string code; // 已校验的协议错误码
};
std::string ResponseHeader(Http* http,const char* lower,const char* canonical) {
    std::string value=http->GetResponseHeader(lower);
    return value.empty()?http->GetResponseHeader(canonical):value;
}
bool SafeToken(const std::string& value) {
    return !value.empty() && value.size()<=64 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos;
}
void Headers(Http* http,bool binary=false) {
    char id[40];snprintf(id,sizeof(id),"%08lx-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    http->SetHeader("X-Request-Id",id);
    http->SetHeader("Accept",binary ? "application/octet-stream":"application/json");
    http->SetHeader("Accept-Language",Lang::Current());
    http->SetHeader("Accept-Encoding","identity");
    http->SetHeader("Connection","close");
}
unsigned RetryAfter(Http* http) {
    const auto value=ResponseHeader(http,"retry-after","Retry-After");
    unsigned seconds=0;
    if(value.empty() || value.size()>9) return 0;
    for(char c:value) {if(c<'0'||c>'9')return 0;seconds=seconds*10+unsigned(c-'0');}
    return seconds;
}
bool WaitRetry(unsigned attempt,unsigned seconds,const std::atomic<bool>& cancelled) {
    // 长等待交还用户，避免商店任务长期占用资源；不提前重试。
    if(seconds>60)return false;
    unsigned ticks=std::max(seconds*10,(1u<<attempt)*10)+esp_random()%5;
    for(unsigned i=0;i<ticks&&!cancelled.load();++i)vTaskDelay(pdMS_TO_TICKS(100));
    return !cancelled.load();
}
bool ReadBody(Http* http,std::string& body,size_t limit,const std::atomic<bool>& cancelled) {
    char buffer[2048];
    while(!cancelled.load()) {
        int n=http->Read(buffer,sizeof(buffer));
        if(n<0)return false;
        if(!n)return true;
        if(size_t(n)>limit-body.size())return false;
        body.append(buffer,n);
    }
    return false;
}
void Problem(const std::string& body,Failure& failure,std::string& error) {
    failure.retryable=failure.status<0||failure.status==429||failure.status>=500;
    Json root(cJSON_ParseWithLengthOpts(body.c_str(),body.size()+1,nullptr,true),cJSON_Delete);
    auto* status=cJSON_GetObjectItemCaseSensitive(root.get(),"status");
    auto* retryable=cJSON_GetObjectItemCaseSensitive(root.get(),"retryable");
    std::string request_id;
    if(cJSON_IsNumber(status)&&status->valuedouble==failure.status&&Text(root.get(),"code",failure.code,64)&&SafeToken(failure.code)) {
        if(cJSON_IsBool(retryable))failure.retryable=failure.retryable&&cJSON_IsTrue(retryable);
        Text(root.get(),"requestId",request_id,64);
    } else failure.code.clear();
    const char* known="unknown";
    for(const char* code:{"INVALID_REQUEST","DEVICE_FORBIDDEN","APP_NOT_AVAILABLE","RELEASE_NOT_FOUND","CATALOG_REVISION_EXPIRED","RELEASE_WITHDRAWN","PACKAGE_CHANGED","RANGE_INVALID","DEVICE_UNSUPPORTED","INTERNAL_ERROR","SERVICE_UNAVAILABLE"})if(failure.code==code){known=code;break;}
    error=Lang::Strings::STORE_NETWORK_ERROR;
    switch(failure.status) {
        case 400:error=Lang::Strings::STORE_INVALID;break;
        case 403:error=Lang::Strings::STORE_FORBIDDEN;break;
        case 404:error=Lang::Strings::STORE_UNAVAILABLE;break;
        case 409:error=Lang::Strings::STORE_CATALOG_EXPIRED;break;
        case 410:error=Lang::Strings::STORE_WITHDRAWN;break;
        case 412:error=Lang::Strings::STORE_PACKAGE_CHANGED;break;
        case 416:error=Lang::Strings::STORE_RANGE_ERROR;break;
        case 422:error=Lang::Strings::STORE_UNSUPPORTED;break;
        case 429:case 503:error=Lang::Strings::STORE_SERVICE_BUSY;break;
    }
    if(failure.status==400||failure.status>=500)
        ESP_LOGE(TAG,"operation=http result=failed http_status=%d code=%s request_id=%s",failure.status,known,SafeToken(request_id)?request_id.c_str():"unavailable");
    else
        ESP_LOGW(TAG,"operation=http result=failed http_status=%d code=%s request_id=%s",failure.status,known,SafeToken(request_id)?request_id.c_str():"unavailable");
}
std::string EscapeQuery(const std::string& value) {
    std::string out;
    static const char hex[]="0123456789ABCDEF";
    for (unsigned char c:value) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.') out+=c;
        else { out+='%';out+=hex[c>>4];out+=hex[c&15]; }
    }
    return out;
}
std::string DeviceQuery(const Item& item) {
    return "?boardName="+EscapeQuery(BOARD_NAME)+"&chipName="+EscapeQuery(SystemInfo::GetChipModelName())+"&mac="+EscapeQuery(api::DeviceId())+"&appId="+EscapeQuery(item.app_id);
}
Json Request(const Item* item=nullptr) {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    auto* device=root ? cJSON_AddObjectToObject(root.get(),"device") : nullptr;
    const std::string chip=SystemInfo::GetChipModelName();
    const auto& mac=api::DeviceId();
    unsigned parts[3];
    if (!Version(esp_app_get_description()->version,parts) || mac.size()!=17 || mac=="00:00:00:00:00:00" || mac=="ff:ff:ff:ff:ff:ff" || !device ||
        !cJSON_AddNumberToObject(root.get(),"schemaVersion",1) ||
        !cJSON_AddStringToObject(device,"boardName",BOARD_NAME) ||
        !cJSON_AddStringToObject(device,"chipName",chip.c_str()) ||
        !cJSON_AddStringToObject(device,"mac",mac.c_str()) ||
        !cJSON_AddStringToObject(device,"firmwareVersion",esp_app_get_description()->version) ||
        !cJSON_AddNumberToObject(device,"appAbiVersion",1) ||
        (item && (!cJSON_AddStringToObject(root.get(),"appId",item->app_id.c_str()) ||
                  !cJSON_AddStringToObject(root.get(),"releaseId",item->id.c_str())))) {ESP_LOGE(TAG,"operation=request stage=device_or_json result=invalid");return Json(nullptr,cJSON_Delete);}
    return root;
}
Json MessageOnce(const std::string& server,const char* route,cJSON* request,std::string& error,const std::atomic<bool>& cancelled,bool get,Failure& failure) {
    if (!request && !get) {error=Lang::Strings::STORE_MEMORY_ERROR;return Json(nullptr,cJSON_Delete);}
    if (!NetworkReady(error,cancelled))return Json(nullptr,cJSON_Delete);
    std::unique_ptr<char,decltype(&cJSON_free)> text(request ? cJSON_PrintUnformatted(request):nullptr,cJSON_free);
    if(!get&&(!text||strlen(text.get())>4096)){error=Lang::Strings::STORE_INVALID;return Json(nullptr,cJSON_Delete);}
    auto* network=Board::GetInstance().GetNetwork();auto http=network?network->CreateHttp(0):nullptr;
    if(!http){ESP_LOGE(TAG,"operation=message stage=create_http result=failed");error=Lang::Strings::STORE_NETWORK_ERROR;return Json(nullptr,cJSON_Delete);}
    Headers(http.get());http->SetTimeout(15000);http->SetRequestDeadline(esp_timer_get_time()+15000000);
    if(text){http->SetHeader("Content-Type","application/json");http->SetContent(std::string(text.get()));}
    bool opened=http->Open(get?"GET":"POST",server+"/"+route);
    failure.status=opened?http->GetStatusCode():-1;
    failure.retry_after=RetryAfter(http.get());
    std::string body;bool ok=opened&&ReadBody(http.get(),body,failure.status==200?256*1024:4096,cancelled);
    const int code=http->GetLastError();if(cancelled.load())http->Cancel();http->Close();
    if(cancelled.load()){error=Lang::Strings::STORE_CANCELLED;return Json(nullptr,cJSON_Delete);}
    if(failure.status!=200){Problem(body,failure,error);return Json(nullptr,cJSON_Delete);}
    if(!ok){ESP_LOGW(TAG,"operation=message stage=read result=failed transport_error=%d",code);error=Lang::Strings::STORE_NETWORK_ERROR;return Json(nullptr,cJSON_Delete);}
    Json response(cJSON_ParseWithLengthOpts(body.c_str(),body.size()+1,nullptr,true),cJSON_Delete);
    auto* schema=cJSON_GetObjectItemCaseSensitive(response.get(),"schemaVersion");
    if(!cJSON_IsNumber(schema)||schema->valuedouble!=1){ESP_LOGE(TAG,"operation=message stage=json result=invalid");error=Lang::Strings::STORE_INVALID;return Json(nullptr,cJSON_Delete);}
    return response;
}
Json Message(const std::string& server,const char* route,cJSON* request,std::string& error,const std::atomic<bool>& cancelled,bool get=false,Failure* result_failure=nullptr) {
    for(unsigned attempt=0;attempt<4&&!cancelled.load();++attempt){
        error.clear();Failure failure;
        auto result=MessageOnce(server,route,request,error,cancelled,get,failure);
        if(result_failure)*result_failure=failure;
        if(result)return result;
        const bool transient=failure.status<0||failure.status==200||failure.status==429||failure.status>=500;
        if(cancelled.load()||!transient||!failure.retryable||error==Lang::Strings::STORE_INVALID||error==Lang::Strings::STORE_MEMORY_ERROR||attempt==3||!WaitRetry(attempt,failure.retry_after,cancelled))break;
    }
    if(cancelled.load())error=Lang::Strings::STORE_CANCELLED;
    return Json(nullptr,cJSON_Delete);
}
static bool WireByteCount(const cJSON* value, size_t& size, size_t minimum = 1) {
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) return false;
    size_t result = 0;
    size_t digits = 0;
    for (const char* cursor = value->valuestring; *cursor; ++cursor) {
        if (++digits > 8 || *cursor < '0' || *cursor > '9') return false;
        const size_t digit = static_cast<size_t>(*cursor - '0');
        if (result > (kMaxImage - digit) / 10) return false;
        result = result * 10 + digit;
    }
    if (result < minimum) return false;
    size = result;
    return true;
}
bool WireItem(cJSON* value,Item& item) {
    if(!cJSON_IsObject(value)) return false;
    size_t package_size = 0;
    if (!WireByteCount(cJSON_GetObjectItemCaseSensitive(value,"size"), package_size)) return false;
    // 网络字符串长度转换为本地安装清单使用的数值格式。
    cJSON_DeleteItemFromObjectCaseSensitive(value,"size");
    if (!cJSON_AddNumberToObject(value,"size",package_size)) return false;
    // 本地清单的兼容字段由当前请求条件补齐。
    for(const char* key:{"description","descriptionEn"})if(!cJSON_GetObjectItemCaseSensitive(value,key)&&!cJSON_AddStringToObject(value,key,""))return false;
    cJSON_DeleteItemFromObjectCaseSensitive(value,"target");
    cJSON_DeleteItemFromObjectCaseSensitive(value,"abiVersion");
    cJSON_DeleteItemFromObjectCaseSensitive(value,"id");
    if(!cJSON_GetObjectItemCaseSensitive(value,"target")&&!cJSON_AddStringToObject(value,"target",CONFIG_IDF_TARGET))return false;
    if(!cJSON_GetObjectItemCaseSensitive(value,"abiVersion")&&!cJSON_AddNumberToObject(value,"abiVersion",1))return false;
    auto* languages=cJSON_GetObjectItemCaseSensitive(value,"languages");
    if(cJSON_IsArray(languages)) {
        if(cJSON_GetArraySize(languages)<1||cJSON_GetArraySize(languages)>8)return false;
        std::string joined;cJSON* language=nullptr;
        cJSON_ArrayForEach(language,languages){if(!cJSON_IsString(language)||!language->valuestring||strlen(language->valuestring)>32)return false;if(!joined.empty())joined+=',';joined+=language->valuestring;}
        cJSON_DeleteItemFromObjectCaseSensitive(value,"languages");
        if(!cJSON_AddStringToObject(value,"languages",joined.c_str()))return false;
    }
    auto* release=cJSON_GetObjectItemCaseSensitive(value,"releaseId");
    if(!cJSON_IsString(release)||!cJSON_AddStringToObject(value,"id",release->valuestring)) return false;
    auto* icon=cJSON_DetachItemFromObjectCaseSensitive(value,"icon");
    Json icon_owner(icon,cJSON_Delete);
    // 图标单独校验，异常时使用默认图标，不使整个目录失效。
    if(!Parse(value,item)) return false;
    item.installed_name.clear();
    if(icon) {
        std::string data,format;
        auto* bytes=cJSON_GetObjectItemCaseSensitive(icon,"size");
        if(Text(icon,"format",format,16)&&format=="xzic"&&Text(icon,"data",data,2752)&&cJSON_IsNumber(bytes)&&bytes->valuedouble==2064) {
            unsigned char bitmap[2064];size_t length=0;
            if(mbedtls_base64_decode(bitmap,sizeof(bitmap),&length,reinterpret_cast<const unsigned char*>(data.data()),data.size())==0) item.icon=AppStoreIcons::Decode(bitmap,length);
        }
        if(!item.icon) ESP_LOGW(TAG,"operation=icon result=fallback");
    }
    return true;
}
}
bool Fetch(const std::string& server,std::vector<Item>& items,std::string& error,const std::atomic<bool>& cancelled,CatalogPage* page) {
    CatalogPage next=page ? *page:CatalogPage{};
    next.expired=false;
    if(page)page->expired=false;
    if(!next.number||next.number>UINT32_MAX){error=Lang::Strings::STORE_INVALID;return false;}
    auto request=Request();
    if(!request||!cJSON_AddNumberToObject(request.get(),"page",next.number)||!cJSON_AddNumberToObject(request.get(),"pageSize",kPageSize)||
        (next.number>1&&!cJSON_AddStringToObject(request.get(),"catalogRevision",next.revision.c_str()))) {error=Lang::Strings::STORE_MEMORY_ERROR;return false;}
    ESP_LOGI(TAG,"operation=catalog result=start page=%zu",next.number);
    Failure failure;
    auto root=Message(server,"apps/query",request.get(),error,cancelled,false,&failure);
    auto* list=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"apps"):nullptr;
    auto* total=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"total"):nullptr;
    auto* number=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"page"):nullptr;
    auto* more=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"hasMore"):nullptr;
    auto* page_size=root ? cJSON_GetObjectItemCaseSensitive(root.get(),"pageSize"):nullptr;
    if(!root){if(page&&failure.status==409&&failure.code=="CATALOG_REVISION_EXPIRED")page->expired=true;return false;}
    if(!cJSON_IsArray(list)||cJSON_GetArraySize(list)>kPageSize||!cJSON_IsNumber(page_size)||page_size->valuedouble!=kPageSize||!cJSON_IsNumber(total)||total->valuedouble<0||total->valuedouble>UINT32_MAX||
        total->valuedouble!=static_cast<uint32_t>(total->valuedouble)||!cJSON_IsNumber(number)||number->valuedouble!=next.number||!cJSON_IsBool(more)||
        !Text(root.get(),"catalogRevision",next.revision,128)||next.revision.empty()||(cJSON_IsTrue(more)&&cJSON_GetArraySize(list)==0)) {
        ESP_LOGE(TAG,"operation=catalog stage=schema result=invalid");error=Lang::Strings::STORE_INVALID;return false;
    }
    std::vector<Item> result;cJSON* value=nullptr;
    cJSON_ArrayForEach(value,list) {Item item;if(!WireItem(value,item)||std::any_of(result.begin(),result.end(),[&](const Item& old){return old.app_id==item.app_id;})) {ESP_LOGE(TAG,"operation=catalog stage=item result=invalid");error=Lang::Strings::STORE_INVALID;return false;}result.push_back(std::move(item));}
    if(page&&page->number>1&&next.revision!=page->revision){ESP_LOGE(TAG,"operation=catalog stage=revision result=invalid");error=Lang::Strings::STORE_INVALID;return false;}
    next.total=static_cast<size_t>(total->valuedouble);next.more=cJSON_IsTrue(more);
    const uint64_t end=(static_cast<uint64_t>(next.number)-1)*kPageSize+result.size();
    if((next.more&&end>=next.total)||(!next.more&&end<next.total)){ESP_LOGE(TAG,"operation=catalog stage=pagination result=invalid");error=Lang::Strings::STORE_INVALID;return false;}
    items=std::move(result);if(page)*page=std::move(next);
    ESP_LOGI(TAG,"operation=catalog result=success count=%zu",items.size());return true;
}
bool CheckUpdate(const std::string& server,const Item& current,Item& candidate,bool& available,std::string& error,const std::atomic<bool>& cancelled) {
    auto request=Request(&current);
    if(request)cJSON_DeleteItemFromObjectCaseSensitive(request.get(),"releaseId");
    if(!request||!cJSON_AddStringToObject(request.get(),"currentVersion",current.version.c_str())) {error=Lang::Strings::STORE_MEMORY_ERROR;return false;}
    auto root=Message(server,"apps/check-update",request.get(),error,cancelled);if(!root)return false;
    auto* update=cJSON_GetObjectItemCaseSensitive(root.get(),"hasUpdate");
    auto* release=cJSON_GetObjectItemCaseSensitive(root.get(),"release");
    std::string app;unsigned parts[3];
    if(!Text(root.get(),"appId",app,48)||app!=current.app_id||!cJSON_IsBool(update)||(!cJSON_IsTrue(update)&&!cJSON_IsNull(release))){ESP_LOGE(TAG,"operation=check_update stage=schema result=invalid");error=Lang::Strings::STORE_INVALID;return false;}
    available=false;
    if(cJSON_IsTrue(update)) {
        Item result=current;
        auto* size=cJSON_GetObjectItemCaseSensitive(release,"size");
        if(!Text(release,"releaseId",result.id,32)||!Hex(result.id,32)||!Text(release,"version",result.version,20)||!Version(result.version,parts)||!Newer(result.version,current.version)||
            !Text(release,"sha256",result.sha256,64)||!Hex(result.sha256,64)||!WireByteCount(size,result.size)||
            !Text(release,"releaseNotes",result.release_notes,1024)||!Text(release,"releaseNotesEn",result.release_notes_en,1024)) {
            ESP_LOGE(TAG,"operation=check_update stage=release result=invalid");error=Lang::Strings::STORE_INVALID;return false;
        }
        candidate=std::move(result);available=true;
    }
    return true;
}
bool Detail(const std::string& server,Item& item,std::string& error,const std::atomic<bool>& cancelled) {
    auto request=Request(&item);auto root=Message(server,"apps/detail",request.get(),error,cancelled);if(!root)return false;
    Item result;if(!WireItem(cJSON_GetObjectItemCaseSensitive(root.get(),"app"),result)||result.app_id!=item.app_id||result.id!=item.id||result.sha256!=item.sha256||result.size!=item.size||result.version!=item.version){ESP_LOGE(TAG,"operation=detail stage=item result=invalid");error=Lang::Strings::STORE_INVALID;return false;}
    result.installed_name = item.installed_name;
    item=std::move(result);return true;
}
namespace {
struct FileSource {
    FILE* file; // 校验后保持打开的 ELF 文件
    size_t length; // 清单声明且已校验的文件长度
};
size_t SourceSize(void* context) { return static_cast<FileSource*>(context)->length; }
int SourceRead(void* context, size_t offset, void* buffer, size_t length) {
    auto* source = static_cast<FileSource*>(context);
    if (offset > source->length || length > source->length - offset) return -EINVAL;
    return fseek(source->file, offset, SEEK_SET) == 0 && fread(buffer, 1, length, source->file) == length ? 0 : -EIO;
}
bool ValidateImage(FILE* file,size_t length) {
    FileSource context{file,length};
    const elf_source_t source{SourceSize,SourceRead,nullptr,&context};
    return esp_elf_validate_source(&source)==0;
}
bool BlockDigest(FILE* file,size_t length,const std::string& expected,const std::atomic<bool>& cancelled) {
    psa_hash_operation_t op=PSA_HASH_OPERATION_INIT;
    if(psa_hash_setup(&op,PSA_ALG_SHA_256)!=PSA_SUCCESS)return false;
    unsigned char buffer[4096];bool ok=true;
    while(length&&!cancelled.load()){size_t n=std::min(length,sizeof(buffer));if(fread(buffer,1,n,file)!=n||psa_hash_update(&op,buffer,n)!=PSA_SUCCESS){ok=false;break;}length-=n;}
    clearerr(file);
    if(ok&&!length&&!cancelled.load())return FinishHash(op,expected);
    psa_hash_abort(&op);return false;
}
Json Checkpoint(const Item& item,const std::string& etag,size_t verified) {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    if(!root||!cJSON_AddStringToObject(root.get(),"appId",item.app_id.c_str())||
        !cJSON_AddStringToObject(root.get(),"releaseId",item.id.c_str())||
        !cJSON_AddNumberToObject(root.get(),"size",item.size)||
        !cJSON_AddStringToObject(root.get(),"sha256",item.sha256.c_str())||
        !cJSON_AddStringToObject(root.get(),"etag",etag.c_str())||
        !cJSON_AddNumberToObject(root.get(),"chunkSize",262144)||
        !cJSON_AddNumberToObject(root.get(),"verified",verified))return Json(nullptr,cJSON_Delete);
    return root;
}
size_t ResumePrefix(const std::string& path,const Item& item,const std::string& etag) {
    FILE* file=fopen(path.c_str(),"rb");if(!file)return 0;
    char data[1025];size_t bytes=fread(data,1,sizeof(data),file);bool ok=!ferror(file);fclose(file);
    if(!ok||bytes==sizeof(data))return 0;
    data[bytes]=0;Json root(cJSON_ParseWithLengthOpts(data,bytes+1,nullptr,true),cJSON_Delete);
    std::string app,release,hash,tag;
    auto* size=cJSON_GetObjectItemCaseSensitive(root.get(),"size");
    auto* stride=cJSON_GetObjectItemCaseSensitive(root.get(),"chunkSize");
    auto* verified=cJSON_GetObjectItemCaseSensitive(root.get(),"verified");
    if(!Text(root.get(),"appId",app,48)||app!=item.app_id||!Text(root.get(),"releaseId",release,32)||release!=item.id||
        !Text(root.get(),"sha256",hash,64)||hash!=item.sha256||!Text(root.get(),"etag",tag,96)||tag!=etag||
        !cJSON_IsNumber(size)||size->valuedouble!=item.size||!cJSON_IsNumber(stride)||stride->valuedouble!=262144||
        !cJSON_IsNumber(verified)||verified->valuedouble<0||verified->valuedouble>item.size||verified->valuedouble!=static_cast<size_t>(verified->valuedouble))return 0;
    const size_t prefix=static_cast<size_t>(verified->valuedouble);
    return prefix==item.size||prefix%262144==0?prefix:0;
}
bool SaveCheckpoint(const std::string& path,const Item& item,const std::string& etag,size_t verified) {
    auto root=Checkpoint(item,etag,verified);
    std::unique_ptr<char,decltype(&cJSON_free)> text(root?cJSON_PrintUnformatted(root.get()):nullptr,cJSON_free);
    if(!text)return false;
    const std::string pending=path+".part";
    FILE* file=fopen(pending.c_str(),"wb");
    bool ok=file&&fwrite(text.get(),1,strlen(text.get()),file)==strlen(text.get());
    if(file){if(fflush(file)!=0||fsync(fileno(file))!=0)ok=false;if(fclose(file)!=0)ok=false;}
    std::string error;
    if(ok)ok=reader::CommitDownloadedFile(pending,path,true,error);
    if(!ok)unlink(pending.c_str());
    return ok;
}
void ClearCheckpoint(const std::string& tmp) {
    for(const char* suffix:{".json",".json.part",".json.cloud-backup"}) {
        if(unlink((tmp+suffix).c_str())!=0&&errno!=ENOENT)ESP_LOGW(TAG,"operation=install stage=checkpoint_cleanup result=failed errno=%d",errno);
    }
}
static void ClearWork(const std::string& work) {
    ClearCheckpoint(work + "/download.part");
    for (const char* name : {"download.part", "manifest.json.part"}) {
        if (unlink((work + "/" + name).c_str()) != 0 && errno != ENOENT)
            ESP_LOGW(TAG, "operation=cleanup stage=work_file result=failed errno=%d", errno);
    }
    if (rmdir(work.c_str()) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "operation=cleanup stage=work_directory result=incomplete errno=%d", errno);
}
static bool DeleteStateFiles(const std::string& directory, bool pending_only) {
    std::unique_ptr<DIR, decltype(&closedir)> entries(opendir(directory.c_str()), closedir);
    if (!entries) return errno == ENOENT;
    for (;;) {
        errno = 0;
        const auto* entry = readdir(entries.get());
        if (!entry) { if (errno) return false; break; }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (pending_only && name != "recording.wav.part" &&
            (!name.starts_with("data-") || !name.ends_with(".bin.part"))) continue;
        const auto path = directory + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) return false;
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; return false; }
        if (unlink(path.c_str()) != 0 && errno != ENOENT) return false;
    }
    entries.reset();
    return rmdir(directory.c_str()) == 0 || errno == ENOENT ||
        (pending_only && (errno == ENOTEMPTY || errno == EEXIST));
}
static bool RecoverCheckpoint(const std::string& checkpoint) {
    struct stat st {};
    const auto backup = checkpoint + ".cloud-backup";
    if (stat(backup.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; return false; }
    if (stat(checkpoint.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; return false; }
        return unlink(backup.c_str()) == 0;
    }
    return errno == ENOENT && rename(backup.c_str(), checkpoint.c_str()) == 0;
}
bool Download(const std::string& server,const Item& item,const std::string& tmp,std::string& error,const std::atomic<bool>& cancelled,Progress* progress,Failure& failure) {
    if(progress)progress->bytes.store(0);
    auto request=Request(&item);
    auto manifest=Message(server,"apps/download-manifest",request.get(),error,cancelled,false,&failure);
    if(!manifest)return false;
    auto* package=cJSON_GetObjectItemCaseSensitive(manifest.get(),"package");
    std::string path,chunks_path,etag,hash,release,app,version;
    auto* size=cJSON_GetObjectItemCaseSensitive(package,"size");
    size_t package_size = 0;
    auto* stride=cJSON_GetObjectItemCaseSensitive(package,"chunkSize");
    const std::string expected=server.substr(Origin(server).size())+"/packages/"+item.id;
    if(!Text(manifest.get(),"appId",app,48)||app!=item.app_id||!Text(manifest.get(),"version",version,20)||version!=item.version||
        !Text(manifest.get(),"releaseId",release,32)||release!=item.id||!Text(package,"path",path,256)||path!=expected||
        !Text(package,"chunksPath",chunks_path,256)||chunks_path!=expected+"/chunks"||
        !Text(package,"etag",etag,96)||etag.size()<3||etag.front()!='"'||etag.back()!='"'||etag.find_first_of("\r\n")!=std::string::npos||
        !Text(package,"sha256",hash,64)||hash!=item.sha256||!WireByteCount(size,package_size)||package_size!=item.size||
        !cJSON_IsNumber(stride)||stride->valuedouble!=262144) {
        ESP_LOGE(TAG,"operation=install stage=manifest result=invalid");error=Lang::Strings::STORE_INVALID;return false;
    }
    const std::string query=DeviceQuery(item);
    auto chunks=Message(server,("packages/"+item.id+"/chunks"+query).c_str(),nullptr,error,cancelled,true,&failure);
    if(!chunks)return false;
    auto* list=cJSON_GetObjectItemCaseSensitive(chunks.get(),"chunks");
    stride=cJSON_GetObjectItemCaseSensitive(chunks.get(),"chunkSize");
    if(!Text(chunks.get(),"releaseId",release,32)||release!=item.id||!Text(chunks.get(),"packageSha256",hash,64)||hash!=item.sha256||
        !cJSON_IsNumber(stride)||stride->valuedouble!=262144||!cJSON_IsArray(list)||cJSON_GetArraySize(list)!=static_cast<int>((item.size+262143)/262144)) {
        ESP_LOGE(TAG,"operation=install stage=chunks result=invalid");error=Lang::Strings::STORE_INVALID;return false;
    }
    const std::string checkpoint=tmp+".json";
    if (!RecoverCheckpoint(checkpoint)) {
        ESP_LOGE(TAG, "operation=install stage=checkpoint_recover result=failed errno=%d", errno);
        error=Lang::Strings::STORE_SD_ERROR; return false;
    }
    const size_t prefix=ResumePrefix(checkpoint,item,etag);
    FILE* file=fopen(tmp.c_str(),"r+b");if(!file)file=fopen(tmp.c_str(),"w+b");
    if(!file){ESP_LOGE(TAG,"operation=install stage=file_open result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;return false;}
    size_t total=0;bool ok=true;char buffer[4096];cJSON* block=nullptr;
    cJSON_ArrayForEach(block,list) {
        const size_t length=std::min<size_t>(262144,item.size-total);
        auto* offset=cJSON_GetObjectItemCaseSensitive(block,"offset");auto* bytes=cJSON_GetObjectItemCaseSensitive(block,"length");auto* index=cJSON_GetObjectItemCaseSensitive(block,"index");
        std::string digest;
        size_t block_offset = 0;
        if(!WireByteCount(offset,block_offset,0)||block_offset!=total||!cJSON_IsNumber(bytes)||bytes->valuedouble!=length||!cJSON_IsNumber(index)||index->valuedouble!=total/262144||!Text(block,"sha256",digest,64)||!Hex(digest,64)) {
            ESP_LOGE(TAG,"operation=install stage=chunk_schema result=invalid");error=Lang::Strings::STORE_INVALID;ok=false;break;
        }
        bool complete=total+length<=prefix&&fseek(file,total,SEEK_SET)==0&&BlockDigest(file,length,digest,cancelled);
        unsigned retry_after=0;
        for(unsigned attempt=0;!complete&&attempt<4&&!cancelled.load();++attempt) {
            if(attempt&&!WaitRetry(attempt-1,retry_after,cancelled))break;
            if(fflush(file)!=0||ftruncate(fileno(file),total)!=0||fseek(file,total,SEEK_SET)!=0){ESP_LOGE(TAG,"operation=install stage=seek_truncate result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;break;}
            auto* network=Board::GetInstance().GetNetwork();auto http=network?network->CreateHttp(0):nullptr;
            if(!http){ESP_LOGE(TAG,"operation=install stage=create_http result=failed");error=Lang::Strings::STORE_NETWORK_ERROR;break;}
            Headers(http.get(),true);http->SetTimeout(20000);http->SetHeader("If-Match",etag);
            char range[96];snprintf(range,sizeof(range),"bytes=%zu-%zu",total,total+length-1);http->SetHeader("Range",range);
            bool opened=http->Open("GET",Origin(server)+path+query);failure=Failure{};failure.status=opened?http->GetStatusCode():-1;
            retry_after=RetryAfter(http.get());
            char expected_range[96];snprintf(expected_range,sizeof(expected_range),"bytes %zu-%zu/%zu",total,total+length-1,item.size);
            bool valid=opened&&failure.status==206&&ResponseHeader(http.get(),"content-range","Content-Range")==expected_range&&ResponseHeader(http.get(),"etag","ETag")==etag;
            const auto encoding=ResponseHeader(http.get(),"content-encoding","Content-Encoding");
            valid=valid&&(encoding.empty()||encoding=="identity");
            psa_hash_operation_t hash_op=PSA_HASH_OPERATION_INIT;
            if(valid&&psa_hash_setup(&hash_op,PSA_ALG_SHA_256)!=PSA_SUCCESS){ESP_LOGE(TAG,"operation=install stage=hash_setup result=failed");valid=false;}
            size_t received=0;bool storage=false;
            while(valid&&!cancelled.load()) {
                int n=http->Read(buffer,sizeof(buffer));if(n<0){valid=false;break;}if(!n)break;
                if(static_cast<size_t>(n)>length-received){valid=false;break;}
                if(fwrite(buffer,1,n,file)!=static_cast<size_t>(n)){storage=true;break;}
                if(psa_hash_update(&hash_op,reinterpret_cast<const unsigned char*>(buffer),n)!=PSA_SUCCESS){ESP_LOGE(TAG,"operation=install stage=hash_update result=failed");valid=false;break;}
                received+=n;
            }
            if(failure.status!=206) {
                std::string body;if(opened&&failure.status!=200)ReadBody(http.get(),body,4096,cancelled);
                if(!cancelled.load())Problem(body,failure,error);
                if(failure.status==416) {
                    const std::string expected_total="bytes */"+std::to_string(item.size);
                    ESP_LOGW(TAG,"operation=install stage=range result=invalid total_matches=%d",ResponseHeader(http.get(),"content-range","Content-Range")==expected_total);
                    ClearCheckpoint(tmp);
                }
            }
            const int transport=http->GetLastError();if(cancelled.load())http->Cancel();http->Close();
            if(fflush(file)!=0)storage=true;
            if(storage){psa_hash_abort(&hash_op);ESP_LOGE(TAG,"operation=install stage=write result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;break;}
            if(valid&&received==length&&!cancelled.load())complete=FinishHash(hash_op,digest);
            else psa_hash_abort(&hash_op);
            if(complete){error.clear();break;}
            if(cancelled.load())break;
            if(failure.status==206)error=Lang::Strings::STORE_VERIFY_ERROR;
            ESP_LOGW(TAG,"operation=install stage=chunk result=incomplete http_status=%d transport_error=%d offset=%zu attempt=%u",failure.status,transport,total,attempt+1);
            if(!failure.retryable||(failure.status>0&&failure.status!=206&&failure.status!=429&&failure.status<500))break;
        }
        if(!complete||cancelled.load()){ok=false;break;}
        if(fflush(file)!=0||fsync(fileno(file))!=0||!SaveCheckpoint(checkpoint,item,etag,total+length)) {
            ESP_LOGE(TAG,"operation=install stage=flush_checkpoint result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;ok=false;break;
        }
        total+=length;if(progress)progress->bytes.store(total);
    }
    if(ok&&(fflush(file)!=0||ftruncate(fileno(file),item.size)!=0)){ESP_LOGE(TAG,"operation=install stage=final_flush result=failed errno=%d",errno);ok=false;error=Lang::Strings::STORE_SD_ERROR;}
    if(fclose(file)!=0){ESP_LOGE(TAG,"operation=install stage=file_close result=failed errno=%d",errno);ok=false;error=Lang::Strings::STORE_SD_ERROR;}
    if(cancelled.load())ESP_LOGI(TAG,"operation=install stage=download result=cancelled bytes=%zu",total);
    else if(!ok&&error.empty())error=Lang::Strings::STORE_NETWORK_ERROR;
    return ok&&!cancelled.load();
}

}
bool Install(const std::string& server,const Item& requested,std::string& error,const std::atomic<bool>& cancelled, Progress* progress) {
    Item item=requested;
    if(cancelled.load()) { ESP_LOGI(TAG, "operation=install result=cancelled"); error=Lang::Strings::STORE_CANCELLED; return false; }
    const int64_t started=esp_timer_get_time();
    ESP_LOGI(TAG, "operation=install result=start app=%s bytes=%zu",Identifier(item.app_id) ? item.app_id.c_str() : "invalid",item.size);
    if(progress) progress->total.store(item.size);
    if(!NetworkReady(error,cancelled)) return false;
    if(!Detail(server,item,error,cancelled))return false;
    std::vector<Item> installed;
    if(!Scan(installed,error)) return false;
    auto old=std::find_if(installed.begin(),installed.end(),[&](const Item& v){return v.app_id==item.app_id;});
    if(old!=installed.end() && !Newer(item.version,old->version)) { ESP_LOGI(TAG, "operation=install result=already_current"); error=Lang::Strings::STORE_CURRENT; return false; }
    sd_access::Transaction transaction;
    item.installed_name = old == installed.end() ? InstallName(item) : InstallName(*old);
    if (std::any_of(installed.begin(), installed.end(), [&](const Item& other) {
        return other.app_id != item.app_id && strcasecmp(InstallName(other).c_str(), item.installed_name.c_str()) == 0;
    })) { ESP_LOGE(TAG,"operation=install stage=name_conflict result=failed"); error=Lang::Strings::STORE_INVALID; return false; }
    const std::string dir=InstallDirectory(item);
    if(!transaction) { ESP_LOGE(TAG, "operation=install stage=sd_access result=failed"); error=Lang::Strings::STORE_SD_ERROR; return false; }
    if(!Directory(dir)) { ESP_LOGE(TAG, "operation=install stage=directory result=failed errno=%d",errno); error=Lang::Strings::STORE_SD_ERROR; return false; }
    const auto work = WorkDirectory(item);
    if (work.empty() || !Directory(work)) {
        ESP_LOGE(TAG, "operation=install stage=work_directory result=failed errno=%d", errno);
        error=Lang::Strings::STORE_SD_ERROR; return false;
    }
    const std::string tmp=work+"/download.part", image=ImagePath(item);
    Failure failure;bool downloaded=false;
    for(unsigned attempt=0;attempt<4&&!cancelled.load();++attempt) {
        error.clear();failure=Failure{};
        downloaded=Download(server,item,tmp,error,cancelled,progress,failure);
        if(downloaded||failure.status!=412||attempt==3)break;
        ESP_LOGW(TAG,"operation=install stage=manifest result=refresh attempt=%u",attempt+1);
    }
    if(!downloaded){if(cancelled.load())error=Lang::Strings::STORE_CANCELLED;return false;}
    FILE* f=nullptr;bool ok=false;
    if(progress)progress->verifying.store(true);
    f=fopen(tmp.c_str(),"rb");ok=f&&Digest(f,item.size,item.sha256,&cancelled)&&ValidateImage(f,item.size);if(f)fclose(f);
    if(!ok){
        if(cancelled.load()){ESP_LOGI(TAG,"operation=install stage=verify result=cancelled");error=Lang::Strings::STORE_CANCELLED;}
        else {ESP_LOGE(TAG,"operation=install stage=verify result=failed");ClearWork(work);error=Lang::Strings::STORE_VERIFY_ERROR;}
        return false;
    }
    ESP_LOGD(TAG, "operation=install stage=verify result=success bytes=%zu",item.size);
    std::lock_guard<std::mutex> lock(s_storage);
    const bool changed_image=old==installed.end() || old->sha256!=item.sha256;
    const std::string previous = old == installed.end() ? std::string{} : dir+"/"+old->sha256+".elf";
    bool saved_previous = false;
    auto rollback_image = [&]() {
        if (!changed_image) return;
        // 未提交的新包移回下载目录，保留下次安装可复用的数据。
        if (rename(image.c_str(), tmp.c_str()) != 0 && errno != ENOENT)
            ESP_LOGE(TAG,"operation=install stage=rollback_download result=failed errno=%d",errno);
        if (saved_previous && rename(previous.c_str(), image.c_str()) != 0)
            ESP_LOGE(TAG,"operation=install stage=rollback_image result=failed errno=%d",errno);
    };
    auto stop_cancelled=[&]() {
        if (!cancelled.load()) return false;
        ESP_LOGI(TAG,"operation=install result=cancelled bytes=%zu",item.size);
        error=Lang::Strings::STORE_CANCELLED;
        return true;
    };
    if (stop_cancelled()) return false;
    f=fopen(image.c_str(),"rb");
    const bool reusable=f && Digest(f,item.size,item.sha256,&cancelled);
    if(f) fclose(f);
    if (stop_cancelled()) return false;
    if(reusable) { unlink(tmp.c_str()); }
    else {
        struct stat st {};
        if (stat(image.c_str(), &st) == 0) {
            if (old == installed.end() || stat(previous.c_str(), &st) == 0 || errno != ENOENT ||
                rename(image.c_str(), previous.c_str()) != 0) {
                ESP_LOGE(TAG,"operation=install stage=image_backup result=failed errno=%d",errno);
                error=Lang::Strings::STORE_SD_ERROR; return false;
            }
            saved_previous = true;
        } else if (errno != ENOENT) {
            ESP_LOGE(TAG,"operation=install stage=image_stat result=failed errno=%d",errno);
            error=Lang::Strings::STORE_SD_ERROR; return false;
        }
        if (rename(tmp.c_str(), image.c_str()) != 0) {
            ESP_LOGE(TAG,"operation=install stage=image_commit result=failed errno=%d",errno);
            if (saved_previous && rename(previous.c_str(), image.c_str()) != 0)
                ESP_LOGE(TAG,"operation=install stage=rollback_image result=failed errno=%d",errno);
            error=Lang::Strings::STORE_SD_ERROR; return false;
        }
    }
    auto json=Serialize(item);
    std::unique_ptr<char,decltype(&cJSON_free)> text(json ? cJSON_PrintUnformatted(json.get()) : nullptr,cJSON_free);
    if (!text) {
        ESP_LOGE(TAG,"operation=install stage=manifest_serialize result=no_memory");
        rollback_image();
        error=Lang::Strings::STORE_MEMORY_ERROR;
        return false;
    }
    const std::string manifest=dir+"/manifest.json", pending=work+"/manifest.json.part";
    f=fopen(pending.c_str(),"wb");
    ok=f && fwrite(text.get(),1,strlen(text.get()),f)==strlen(text.get());
    if(f) { if(fflush(f)!=0) ok=false; if(fclose(f)!=0) ok=false; }
    if (!ok) {
        ESP_LOGE(TAG,"operation=install stage=manifest_write result=failed errno=%d",errno);
        unlink(pending.c_str());
        rollback_image();
        error=Lang::Strings::STORE_SD_ERROR;
        return false;
    }
    if (stop_cancelled()) {
        unlink(pending.c_str());
        rollback_image();
        return false;
    }
    // 清单切换是安装提交点；提交后完成清理，不再回滚已安装版本。
    if(!reader::CommitDownloadedFile(pending,manifest,true,error)) {
        ESP_LOGE(TAG, "operation=install stage=manifest_commit result=failed errno=%d",errno);
        unlink(pending.c_str());
        rollback_image();
        error=Lang::Strings::STORE_SD_ERROR; return false;
    }
    ClearWork(work);
    CleanVersions(dir,item.installed_name+".elf");
    ESP_LOGI(TAG, "operation=install result=success bytes=%zu elapsed_ms=%lld",item.size,(long long)((esp_timer_get_time()-started)/1000));
    return true;
}
bool Remove(const Item& item,std::string& error) {
    ESP_LOGI(TAG, "operation=remove result=start");
    std::lock_guard<std::mutex> lock(s_storage);
    sd_access::Transaction transaction;
    if(!transaction) { ESP_LOGE(TAG, "operation=remove stage=sd_access result=failed"); error=Lang::Strings::STORE_SD_ERROR; return false; }
    if (!Identifier(item.app_id) || !ValidInstallName(InstallName(item))) { error=Lang::Strings::STORE_INVALID; return false; }
    const auto storage_key = InstallName(item)+"--"+item.app_id;
    const auto state = std::string(SD_PATH_APP_STATE)+"/"+storage_key;
    const auto state_work = std::string(SD_PATH_APP_STORE_TEMP)+"/"+storage_key;
    const auto work = WorkDirectory(item);
    // 状态删除失败时保留安装清单，使用户可以重试卸载。
    if (!DeleteStateFiles(state, false) || !DeleteStateFiles(state_work, true) ||
        (!work.empty() && work != state_work && !DeleteStateFiles(work, true))) {
        ESP_LOGE(TAG,"operation=remove stage=state_delete result=failed errno=%d",errno);
        error=Lang::Strings::STORE_SD_ERROR; return false;
    }
    const std::string dir=InstallDirectory(item), manifest=dir+"/manifest.json";
    if(unlink((manifest+".cloud-backup").c_str())!=0 && errno!=ENOENT) {ESP_LOGE(TAG, "operation=remove stage=backup_delete result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;return false;}
    if(unlink(manifest.c_str())!=0) {ESP_LOGE(TAG, "operation=remove stage=manifest_delete result=failed errno=%d",errno);error=Lang::Strings::STORE_SD_ERROR;return false;}
    if (unlink(ImagePath(item).c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG,"operation=remove stage=image_delete result=failed errno=%d",errno);
        error=Lang::Strings::STORE_SD_ERROR; return false;
    }
    CleanVersions(dir,"");
    if (!work.empty()) ClearWork(work);
    if(rmdir(dir.c_str())!=0) ESP_LOGW(TAG, "operation=remove stage=directory_cleanup result=incomplete errno=%d",errno);
    ESP_LOGI(TAG, "operation=remove result=success");
    return true;
}
namespace {
bool Assets(FILE* file, size_t length, size_t& offset, size_t& bytes) {
    FileSource source{file, length};
    elf32_hdr_t header{};
    if (SourceRead(&source, 0, &header, sizeof(header)) || memcmp(&header, "\177ELF\1\1", 6) ||
        header.type != 3 || header.machine != 243 || header.shentsize != sizeof(elf32_shdr_t) ||
        !header.shnum || header.shnum > 512 || header.shstrndx >= header.shnum ||
        header.shoff > length || size_t(header.shnum) * sizeof(elf32_shdr_t) > length - header.shoff) return false;
    elf32_shdr_t names{};
    if (SourceRead(&source, header.shoff + size_t(header.shstrndx) * sizeof(names), &names, sizeof(names))) return false;
    if (names.type != 3 || names.offset > length || names.size > length - names.offset) return false;
    bool found = false;
    for (unsigned i = 0; i < header.shnum; ++i) {
        elf32_shdr_t section{};
        if (SourceRead(&source, header.shoff + size_t(i) * sizeof(section), &section, sizeof(section))) return false;
        char name[15];
        if (section.name > names.size || sizeof(name) > names.size - section.name) continue;
        if (SourceRead(&source, size_t(names.offset) + section.name, name, sizeof(name))) return false;
        if (memcmp(name, ".xz.app.assets", sizeof(name))) continue;
        if (found || section.type != 1 || !section.size || (section.flags & (2 | 0x800)) ||
            section.offset > length || section.size > length - section.offset) return false;
        offset = section.offset;
        bytes = section.size;
        found = true;
    }
    return found;
}
}
bool Load(const Item& item, esp_elf_t& elf, std::string& error) {
    ESP_LOGI(TAG, "operation=load result=start app=%s bytes=%zu",Identifier(item.app_id) ? item.app_id.c_str() : "invalid",item.size);
    std::lock_guard<std::mutex> lock(s_storage);
    static bool symbols_ready = false;
    if (!symbols_ready) {
        const int result = esp_elf_register_symbol(kRuntimeMath);
        if (result != 0 && result != -EEXIST) { ESP_LOGE(TAG, "operation=load stage=register_symbols result=failed code=%d",result); error = Lang::Strings::STORE_RUN_ERROR; return false; }
        symbols_ready = true;
    }
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGE(TAG, "operation=load stage=sd_access result=failed"); error = Lang::Strings::STORE_SD_ERROR; return false; }
    FILE* file = fopen(ImagePath(item).c_str(), "rb");
    if (!file) { ESP_LOGE(TAG, "operation=load stage=file_open result=failed errno=%d",errno); error = Lang::Strings::STORE_SD_ERROR; return false; }
    const bool verified = Digest(file, item.size, item.sha256);
    bool ok = verified;
    int relocate_error=0;
    if (ok) {
        FileSource context{file, item.size};
        const elf_source_t source{SourceSize, SourceRead, nullptr, &context};
        relocate_error=esp_elf_relocate_source(&elf, &source);
        ok=relocate_error==0;
    }
    fclose(file);
    if (!ok) {
        ESP_LOGE(TAG, "operation=load stage=%s result=failed code=%d",verified ? "relocate" : "verify",relocate_error);
        error = verified ? Lang::Strings::STORE_RUN_ERROR : Lang::Strings::STORE_VERIFY_ERROR;
    } else ESP_LOGI(TAG, "operation=load result=success");
    return ok;
}
size_t ReadAsset(const Item& item, size_t start, void* buffer, size_t length) {
    std::lock_guard<std::mutex> lock(s_storage);
    sd_access::Transaction transaction;
    if (!buffer || !length) return 0;
    if (!transaction) { ESP_LOGE(TAG, "operation=asset_read stage=sd_access result=failed"); return 0; }
    FILE* file = fopen(ImagePath(item).c_str(), "rb");
    if (!file) { ESP_LOGE(TAG, "operation=asset_read stage=file_open result=failed errno=%d",errno); return 0; }
    size_t offset = 0, bytes = 0;
    const bool ok = Assets(file, item.size, offset, bytes) && start <= bytes && length <= bytes - start &&
        fseek(file, offset + start, SEEK_SET) == 0 && fread(buffer, 1, length, file) == length;
    fclose(file);
    if (!ok) ESP_LOGE(TAG, "operation=asset_read stage=read result=failed offset=%zu bytes=%zu",start,length);
    return ok ? length : 0;
}
size_t AssetSize(const Item& item) {
    std::lock_guard<std::mutex> lock(s_storage);
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGE(TAG, "operation=asset_size stage=sd_access result=failed"); return 0; }
    FILE* file = fopen(ImagePath(item).c_str(), "rb");
    if (!file) { ESP_LOGE(TAG, "operation=asset_size stage=file_open result=failed errno=%d",errno); return 0; }
    size_t offset = 0, bytes = 0;
    if (!Assets(file, item.size, offset, bytes)) bytes = 0;
    fclose(file);
    return bytes;
}
bool LocateAsset(const Item& item, std::string& path, size_t& offset, size_t& length) {
    std::lock_guard<std::mutex> lock(s_storage);
    sd_access::Transaction transaction;
    if (!transaction) return false;
    path = ImagePath(item);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "operation=asset_locate stage=open result=failed errno=%d", errno);
        return false;
    }
    const bool valid = Assets(file, item.size, offset, length);
    fclose(file);
    if (!valid) ESP_LOGE(TAG, "operation=asset_locate stage=section result=invalid");
    return valid;
}
}
