#include "assets/lang_screen.h"
#include "assets/lang_network.h"
#include "ap_transfer.h"
#include "ap_request_worker.h"

#include "sd_paths.h"
#include "SdCardManager.hpp"
#include "reader/book_library.h"

#include "assets/lang_config.h"
#include "power_policy.h"
#include "wifi_station.h"
#include "board.h"
#include "dual_network_board.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_http_server.h>
#include <cJSON.h>
#include <zlib.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_vfs_fat.h>
#include <esp_heap_caps.h>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>

#include <strings.h>

#include <atomic>
#include <new>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

namespace {

constexpr const char* TAG = "ApTransfer";
constexpr size_t kNameCapacity = SD_FILE_NAME_CAPACITY; // UTF-8 名称含终止符，为临时文件前后缀预留 9 字节
constexpr size_t kWebPathCapacity = 512; // 网页路径含终止符
constexpr size_t kFsPathCapacity = kWebPathCapacity + sizeof(SD_MOUNT_POINT); // 挂载路径含终止符
constexpr size_t kTransferPathCapacity = kFsPathCapacity + 9; // 临时文件前后缀
constexpr size_t kEncodedPathCapacity = 3 * kWebPathCapacity; // URL 编码的最坏长度
constexpr const char* kUrl = "http://192.168.4.1/";
constexpr int kMaxEntries = 200;

struct ApEnt {
    char name[256];
    uint64_t size;
    bool dir;
};

SemaphoreHandle_t s_mu = nullptr;
portMUX_TYPE s_mu_init = portMUX_INITIALIZER_UNLOCKED;

void LockRadio() {
    if (s_mu == nullptr) {
        taskENTER_CRITICAL(&s_mu_init);
        if (s_mu == nullptr) {
            s_mu = xSemaphoreCreateRecursiveMutex();
        }
        taskEXIT_CRITICAL(&s_mu_init);
    }
    if (s_mu != nullptr) {
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    }
}

void UnlockRadio() {
    if (s_mu != nullptr) {
        xSemaphoreGiveRecursive(s_mu);
    }
}

httpd_handle_t s_httpd = nullptr;
esp_netif_t* s_ap_netif = nullptr;
bool s_running = false;
bool s_station_service = false; // STA 射频由调用方管理
bool s_paused_sta = false;
bool s_we_inited = false;
bool s_net_held = false;
std::atomic<bool> s_upload_busy{false};
std::atomic<bool> s_storage_failed{false}; // I/O 故障持续到重启，防止继续上传到异常卡
const char* s_upload_target = nullptr; // 受上传状态锁保护，借用活动上传会话的目标路径
std::atomic<bool> s_standby_hold{false}; // 待机期间拒绝把热点再拉起来
std::atomic<bool> s_standby_parked{false};

enum class UploadPhase { Idle, Receiving, Saving, Done, Failed, Cancelled };

struct UploadState {
    char id[32] = {}; // 本次任务标识
    char error[192] = {}; // 最近一次失败说明
    uint64_t total = 0; // 文件总字节，不含 WebSocket 帧头
    uint64_t received = 0; // 已接收文件字节
    uint64_t written = 0; // 已写入文件字节
    UploadPhase phase = UploadPhase::Idle; // 设备处理阶段
    bool cancel = false; // 接收阶段的取消请求
    int fd = -1; // 上传连接，仅在忙碌期间有效
};

UploadState s_upload;
StaticSemaphore_t s_upload_lock_storage;
SemaphoreHandle_t s_upload_lock = nullptr;
std::atomic<bool> s_http_stopping{false};

ApRequestWorker s_upload_worker, s_file_worker;
StaticSemaphore_t s_file_lock_storage, s_upload_slots_storage, s_clients_lock_storage;
SemaphoreHandle_t s_file_lock = nullptr, s_upload_slots = nullptr, s_clients_lock = nullptr;
int s_clients[ApRequestWorker::kMaxClients]{}; // 由连接登记锁保护，退出时唤醒包括未完成报头的连接

class FileOperationGuard {
public:
    FileOperationGuard() { xSemaphoreTakeRecursive(s_file_lock, portMAX_DELAY); }
    FileOperationGuard(const FileOperationGuard&) = delete;
    FileOperationGuard& operator=(const FileOperationGuard&) = delete;
    ~FileOperationGuard() { Unlock(); }
    void Unlock() {
        if (locked_) { locked_ = false; xSemaphoreGiveRecursive(s_file_lock); }
    }
private:
    bool locked_ = true; // 可在发送目录快照前提前释放

};

esp_err_t OpenClient(httpd_handle_t, int fd) {
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    bool added = false;
    for (auto& client : s_clients) if (client < 0) { client = fd; added = true; break; }
    // 保持 SDK 对关闭的单一所有权；退出期间新接入的连接也立即唤醒读写。
    if (!added || s_http_stopping.load()) shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(s_clients_lock);
    return ESP_OK;
}

void CloseClient(httpd_handle_t, int fd) {
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (auto& client : s_clients) if (client == fd) { client = -1; break; }
    close(fd);
    xSemaphoreGive(s_clients_lock);
}

void ShutdownClients() {
    if (!s_clients_lock) return;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (const auto fd : s_clients) if (fd >= 0) shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(s_clients_lock);
}

UploadState UploadSnapshot() {
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    const UploadState result = s_upload;
    xSemaphoreGive(s_upload_lock);
    return result;
}

extern const uint8_t kPageStart[] asm("_binary_ap_fileman_html_gz_start");
extern const uint8_t kPageEnd[] asm("_binary_ap_fileman_html_gz_end");

void HoldPower() {
    if (!s_net_held) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
        s_net_held = true;
    }
}

void ReleasePower() {
    if (s_net_held) {
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
        s_net_held = false;
    }
}

void FillSsid(char* out, size_t n) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, n, "Metalio-%02X%02X", mac[4], mac[5]);
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool UrlDecode(const char* in, char* out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            const int hi = HexVal(in[i + 1]);
            const int lo = in[i + 1] ? HexVal(in[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0' || o + 1 >= n) {
            return false;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

bool IsTransferName(const char* name) {
    return strncasecmp(name, ".cz-", 4) == 0;
}

bool SafeName(const char* name) {
    return SdIsValidFileName(name) && !IsTransferName(name);
}

bool WebToFs(const char* web, char* out, size_t n) {
    if (std::snprintf(out, n, "%s", SD_MOUNT_POINT) >= static_cast<int>(n)) {
        return false;
    }
    if (web == nullptr || web[0] != '/' || std::strlen(web) >= kWebPathCapacity) return false;
    const char* p = web;
    while (*p != '\0') {
        while (*p == '/') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char* start = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        const size_t len = static_cast<size_t>(p - start);
        if (start[len - 1] == '.' || start[len - 1] == ' ' ||
            (len >= 4 && strncasecmp(start, ".cz-", 4) == 0)) return false;
        for (size_t i = 0; i < len; ++i) {
            if (static_cast<unsigned char>(start[i]) < 32 || std::strchr("\\:*?\"<>|", start[i])) return false;
        }
        if (std::strlen(out) + 1 + len + 1 > n) {
            return false;
        }
        std::strcat(out, "/");
        std::strncat(out, start, len);
    }
    return true;
}

bool QueryValue(httpd_req_t* req, const char* key, char* out, size_t n, const char* fallback) {
    const size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) {
        return std::snprintf(out, n, "%s", fallback) < static_cast<int>(n);
    }
    char query[kEncodedPathCapacity + 32];
    if (len >= sizeof(query) || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char raw[kEncodedPathCapacity];
    const esp_err_t err = httpd_query_key_value(query, key, raw, sizeof(raw));
    if (err == ESP_ERR_NOT_FOUND) return std::snprintf(out, n, "%s", fallback) < static_cast<int>(n);
    return err == ESP_OK && UrlDecode(raw, out, n);
}

bool ReadForm(httpd_req_t* req, char* body, size_t n) {
    if (req->content_len == 0 || req->content_len >= n) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    body[got] = '\0';
    return true;
}

bool FormValue(const char* body, const char* key, char* out, size_t n) {
    const size_t klen = std::strlen(key);
    const char* p = body;
    while (p != nullptr && *p != '\0') {
        if ((p == body || p[-1] == '&') && std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            char raw[kEncodedPathCapacity];
            size_t i = 0;
            while (*p != '\0' && *p != '&' && i + 1 < sizeof(raw)) {
                raw[i++] = *p++;
            }
            if (*p != '\0' && *p != '&') return false;
            raw[i] = '\0';
            return UrlDecode(raw, out, n);
        }
        p = std::strchr(p, '&');
        if (p != nullptr) {
            ++p;
        }
    }
    return false;
}

esp_err_t SendText(httpd_req_t* req, const char* status, const char* msg) {
    if (status != nullptr) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const esp_err_t err = httpd_resp_sendstr(req, msg);
    // 错误时关闭连接，避免未消费的上传体被当作下一条请求。
    return status != nullptr && status[0] >= '4' ? ESP_FAIL : err;
}

esp_err_t HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(kPageStart), kPageEnd - kPageStart);
}

bool AffectsUpload(const char* path) {
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    const size_t size = std::strlen(path);
    const bool affects = s_upload_busy.load() && s_upload_target != nullptr &&
        strncasecmp(path, s_upload_target, size) == 0 &&
        (s_upload_target[size] == '\0' || s_upload_target[size] == '/');
    xSemaphoreGive(s_upload_lock);
    return affects;
}

bool RecoverUploads(const char* dir) {
    DIR* dp = opendir(dir);
    if (dp == nullptr) return false;
    bool ok = true;
    bool restored = false;
    while (true) {
        if (s_http_stopping.load()) { ok = false; break; }
        errno = 0;
        dirent* entry = readdir(dp);
        if (entry == nullptr) { if (errno != 0) ok = false; break; }
        if (!IsTransferName(entry->d_name)) continue;
        char transfer[kTransferPathCapacity], target[kFsPathCapacity];
        const char* name = entry->d_name + 4;
        const size_t len = std::strlen(name);
        if (len < 5) continue;
        if (std::snprintf(transfer, sizeof(transfer), "%s/%s", dir, entry->d_name) >= static_cast<int>(sizeof(transfer))) {
            ok = false; break;
        }
        const size_t suffix = std::strcmp(name + len - 4, ".bak") == 0 ? 4 :
            std::strcmp(name + len - 5, ".part") == 0 ? 5 : 0;
        if (suffix == 0) continue;
        char original[kNameCapacity];
        if (len - suffix >= sizeof(original)) { ok = false; break; }
        std::memcpy(original, name, len - suffix); original[len - suffix] = '\0';
        if (!SafeName(original)) { ok = false; break; }
        std::snprintf(target, sizeof(target), "%s/%s", dir, original);
        // 跳过活动目标，其余遗留文件仍恢复，防止管理操作覆盖未恢复的原文件。
        if (AffectsUpload(target)) continue;
        if (suffix == 4) {
            struct stat st;
            if (stat(target, &st) == 0) {
                ok = !S_ISDIR(st.st_mode) && unlink(transfer) == 0;
            } else {
                ok = errno == ENOENT && rename(transfer, target) == 0;
                restored = restored || ok;
            }
        } else {
            ok = unlink(transfer) == 0;
        }
        if (!ok) break;
    }
    if (closedir(dp) != 0) ok = false;
    if (restored) reader::InvalidateBookLibraryCache();
    return ok;
}

esp_err_t HandleStorage(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    if (!SdCardManager::GetInstance().IsMounted()) return httpd_resp_sendstr(req, "{\"mounted\":false}");
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free) != ESP_OK) {
        return SendText(req, "500 Internal Server Error", "SD 卡容量读取失败");
    }
    char json[128];
    std::snprintf(json, sizeof(json), "{\"mounted\":true,\"total\":%llu,\"free\":%llu}",
                  static_cast<unsigned long long>(total), static_cast<unsigned long long>(free));
    return httpd_resp_sendstr(req, json);
}

int CmpEnt(const void* a, const void* b) {
    const auto* ea = static_cast<const ApEnt*>(a);
    const auto* eb = static_cast<const ApEnt*>(b);
    if (ea->dir != eb->dir) {
        return ea->dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

esp_err_t HandleFileList(httpd_req_t* req) {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return SendText(req, "503 Service Unavailable", "未检测到内存卡");
    }
    char web[kWebPathCapacity];
    char fs[kFsPathCapacity];
    if (!QueryValue(req, "path", web, sizeof(web), "/") || !WebToFs(web, fs, sizeof(fs))) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (!RecoverUploads(fs)) return SendText(req, "500 Internal Server Error", "目录读取或上传恢复失败");
    DIR* dir = opendir(fs);
    if (dir == nullptr) {
        return SendText(req, "404 Not Found", "目录不存在");
    }
    auto* ents = static_cast<ApEnt*>(heap_caps_malloc(sizeof(ApEnt) * kMaxEntries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ents == nullptr) {
        closedir(dir);
        return SendText(req, "500 Internal Server Error", "内存不足");
    }
    int count = 0;
    bool truncated = false;
    bool read_failed = false;
    while (true) {
        if (s_http_stopping.load()) { read_failed = true; break; }
        errno = 0;
        dirent* ent = readdir(dir);
        if (ent == nullptr) { read_failed = errno != 0; break; }
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (IsTransferName(ent->d_name)) continue;
        if (count >= kMaxEntries) {
            truncated = true;
            break;
        }
        char child[kFsPathCapacity];
        if (std::snprintf(child, sizeof(child), "%s/%s", fs, ent->d_name) >= static_cast<int>(sizeof(child))) {
            truncated = true; continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            read_failed = true; break;
        }
        std::snprintf(ents[count].name, sizeof(ents[count].name), "%s", ent->d_name);
        ents[count].dir = S_ISDIR(st.st_mode);
        ents[count].size = ents[count].dir ? 0 : static_cast<uint64_t>(st.st_size);
        ++count;
    }
    if (closedir(dir) != 0) read_failed = true;
    if (read_failed) {
        std::free(ents);
        return SendText(req, "500 Internal Server Error", "目录读取失败，请检查 SD 卡后刷新");
    }
    guard.Unlock();
    std::qsort(ents, static_cast<size_t>(count), sizeof(ApEnt), CmpEnt);

    httpd_resp_set_type(req, "application/json");
    char head[32];
    std::snprintf(head, sizeof(head), "{\"truncated\":%s,\"items\":[", truncated ? "true" : "false");
    esp_err_t send_err = httpd_resp_sendstr_chunk(req, head);
    for (int i = 0; i < count && send_err == ESP_OK; ++i) {
        char esc[1536];
        size_t o = 0;
        for (size_t c = 0; ents[i].name[c] != '\0' && o + 2 < sizeof(esc); ++c) {
            const unsigned char ch = static_cast<unsigned char>(ents[i].name[c]);
            if (ch < 32) {
                if (o + 6 >= sizeof(esc)) break;
                std::snprintf(esc + o, sizeof(esc) - o, "\\u%04x", ch); o += 6; continue;
            }
            if (ch == '"' || ch == '\\') {
                esc[o++] = '\\';
            }
            esc[o++] = ch;
        }
        esc[o] = '\0';
        char line[1664];
        std::snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}", i ? "," : "",
                      esc, ents[i].dir ? "true" : "false",
                      static_cast<unsigned long long>(ents[i].size));
        send_err = httpd_resp_sendstr_chunk(req, line);
    }
    if (send_err == ESP_OK) send_err = httpd_resp_sendstr_chunk(req, "]}");
    if (send_err == ESP_OK) send_err = httpd_resp_sendstr_chunk(req, nullptr);
    std::free(ents);
    return send_err;
}

esp_err_t HandleDownload(httpd_req_t* req) {
    char web[kWebPathCapacity];
    char fs[kFsPathCapacity];
    if (!QueryValue(req, "path", web, sizeof(web), "") || web[0] == '\0' ||
        !WebToFs(web, fs, sizeof(fs))) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (s_upload_busy.load()) return SendText(req, "503 Service Unavailable", "上传期间暂不支持下载，请完成后重试");
    struct stat st;
    if (stat(fs, &st) != 0 || S_ISDIR(st.st_mode)) {
        return SendText(req, "404 Not Found", "文件不存在");
    }
    FILE* fp = std::fopen(fs, "rb");
    if (fp == nullptr) {
        return SendText(req, "500 Internal Server Error", "无法读取");
    }
    const char* base = std::strrchr(web, '/');
    base = (base != nullptr && base[1] != '\0') ? base + 1 : web;
    char disp[3 * kNameCapacity + 32];
    size_t used = std::snprintf(disp, sizeof(disp), "attachment; filename*=UTF-8''");
    const char* hex = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(base); *p && used + 3 < sizeof(disp); ++p) {
        disp[used++] = '%'; disp[used++] = hex[*p >> 4]; disp[used++] = hex[*p & 15];
    }
    disp[used] = '\0';
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char buf[1024];
    bool failed = false;
    while (true) {
        if (s_http_stopping.load()) { failed = true; break; }
        const size_t n = std::fread(buf, 1, sizeof(buf), fp);
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            failed = true;
            break;
        }
    }
    if (std::ferror(fp)) failed = true;
    if (std::fclose(fp) != 0) failed = true;
    if (!failed) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return failed ? ESP_FAIL : ESP_OK;
}

void UploadWriteError(int code, const char*& error) {
    if (code == ENOSPC) {
        error = "SD 卡空间不足，原文件保留";
    } else if (code == ENOMEM) {
        error = "设备内存不足，上传已停止，原文件保留；请稍后重试";
    } else if (code == EIO) {
        s_storage_failed.store(true);
        error = "内存卡状态异常，请重启设备后再试";
    } else {
        error = "SD 卡写入失败，原文件保留；请检查内存卡后重试";
    }
    ESP_LOGE(TAG, "upload storage error errno=%d", code);
}

constexpr size_t kUploadChunk = 4096;
constexpr size_t kUploadDecodeChunk = 16384;
constexpr uint64_t kUploadProgressBytes = 65536;
#if CONFIG_SPIRAM
constexpr uint32_t kUploadMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kUploadMemoryCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif

struct UploadSession {
    uint64_t wire_bytes = 0; // 已收到的二进制载荷，不含 WebSocket 帧头
    FILE* file = nullptr; // 当前临时文件，仅由上传任务访问
    uint8_t* buffer = nullptr; // 本连接复用的分块接收缓冲
    uint8_t* decoded = nullptr; // 按需分配的解压输出缓冲
    z_stream inflater = {}; // 本连接复用的 zlib 状态
    bool zlib_stream = false; // 是否使用连续 zlib 流
    bool stream_end = false; // 是否已通过流结束及校验和检查
    char path[kFsPathCapacity] = {}; // 最终文件路径
    char temp[kTransferPathCapacity] = {}; // 同目录临时文件
    char backup[kTransferPathCapacity] = {}; // 覆盖前的备份
    bool replace = false; // 是否明确允许覆盖
    int fd = -1; // 本会话的 WebSocket
};

voidpf UploadZalloc(voidpf, uInt count, uInt size) {
    return heap_caps_calloc(count, size, kUploadMemoryCaps);
}

void UploadZfree(voidpf, voidpf pointer) {
    heap_caps_free(pointer);
}

int WriteUploadData(UploadSession* session, const uint8_t* data, size_t length) {
    const auto snapshot = UploadSnapshot();
    if (snapshot.cancel || s_http_stopping.load() || length > snapshot.total - snapshot.received) return Z_DATA_ERROR;
    errno = 0;
    const size_t written = std::fwrite(data, 1, length, session->file);
    const int code = errno;
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    s_upload.received += length;
    s_upload.written += written;
    xSemaphoreGive(s_upload_lock);
    if (written == length) return Z_OK;
    errno = code;
    return Z_ERRNO;
}

int InflateUploadData(UploadSession* session, uint8_t* input, size_t size) {
    if (session->stream_end) return Z_DATA_ERROR;
    if (session->decoded == nullptr) {
        session->decoded = static_cast<uint8_t*>(heap_caps_malloc(kUploadDecodeChunk, kUploadMemoryCaps));
        if (session->decoded == nullptr) return Z_MEM_ERROR;
    }
    auto& z = session->inflater;
    if (z.state == nullptr) {
        z.zalloc = UploadZalloc;
        z.zfree = UploadZfree;
        const int result = inflateInit(&z);
        if (result != Z_OK) return result;
    }
    z.next_in = input;
    z.avail_in = size;
    // 跨帧保留字典；高压缩率帧也只用固定输出缓冲逐批写入。
    do {
        z.next_out = session->decoded;
        z.avail_out = kUploadDecodeChunk;
            const int result = inflate(&z, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) return result;
        const size_t length = kUploadDecodeChunk - z.avail_out;
        if (length) {
            const int written = WriteUploadData(session, session->decoded, length);
            if (written != Z_OK) return written;
        }
        if (result == Z_STREAM_END) {
            session->stream_end = true;
            return z.avail_in == 0 ? Z_OK : Z_DATA_ERROR;
        }
        if (result == Z_BUF_ERROR) return z.avail_in == 0 ? Z_OK : Z_DATA_ERROR;
    } while (z.avail_in || z.avail_out == 0);
    return Z_OK;
}

bool OwnsUpload(const UploadSession* session) {
    return s_upload_busy.load() && UploadSnapshot().fd == session->fd;
}

void ReleaseUpload(UploadSession* session, const char* error, bool cancelled = false) {
    FileOperationGuard guard;
    if (session->file != nullptr) { std::fclose(session->file); session->file = nullptr; }
    if (session->temp[0]) unlink(session->temp);
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    s_upload.phase = cancelled ? UploadPhase::Cancelled : UploadPhase::Failed;
    std::snprintf(s_upload.error, sizeof(s_upload.error), "%s", error);
    s_upload_target = nullptr;
    s_upload_busy.store(false);
    xSemaphoreGive(s_upload_lock);
}

void DestroyUploadSession(void* context) {
    FileOperationGuard guard;
    auto* session = static_cast<UploadSession*>(context);
    if (OwnsUpload(session)) ReleaseUpload(session, "连接中断，未提交的文件已清理，原文件保留");
    if (session->inflater.state) inflateEnd(&session->inflater);
    heap_caps_free(session->decoded);
    heap_caps_free(session->buffer);
    session->~UploadSession();
    heap_caps_free(session);
    xSemaphoreGive(s_upload_slots);
}

void FreeUploadSession(void* context) {
    if (!s_upload_worker.Cleanup(DestroyUploadSession, context)) DestroyUploadSession(context);
}

esp_err_t PushUpload(httpd_req_t* req, const char* event = "progress") {
    const auto snapshot = UploadSnapshot();
    static const char* phases[] = {"idle", "receiving", "saving", "done", "failed", "cancelled"};
    char escaped[sizeof(snapshot.error) * 2];
    size_t n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(snapshot.error); *p && n + 2 < sizeof(escaped); ++p) {
        if (*p < 32) { escaped[n++] = ' '; continue; }
        if (*p == '"' || *p == '\\') escaped[n++] = '\\';
        escaped[n++] = *p;
    }
    escaped[n] = '\0';
    char json[768];
    std::snprintf(json, sizeof(json), "{\"event\":\"%s\",\"id\":\"%s\",\"state\":\"%s\",\"total\":%llu,\"received\":%llu,\"written\":%llu,\"active\":%s,\"error\":\"%s\",\"chunk\":%u,\"encoding\":\"%s\",\"wire\":%llu}",
                  event, snapshot.id, phases[static_cast<unsigned>(snapshot.phase)],
                  static_cast<unsigned long long>(snapshot.total), static_cast<unsigned long long>(snapshot.received),
                  static_cast<unsigned long long>(snapshot.written), s_upload_busy.load() ? "true" : "false", escaped,
                  static_cast<unsigned>(kUploadChunk),
                  static_cast<UploadSession*>(req->sess_ctx)->zlib_stream ? "zlib-stream-v1" : "identity",
                  static_cast<unsigned long long>(static_cast<UploadSession*>(req->sess_ctx)->wire_bytes));
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(json);
    frame.len = std::strlen(json);
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t RejectUpload(httpd_req_t* req, const char* error) {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) return ESP_ERR_NO_MEM;
    const bool valid = cJSON_AddStringToObject(json, "event", "error") && cJSON_AddStringToObject(json, "error", error);
    char* text = valid ? cJSON_PrintUnformatted(json) : nullptr;
    cJSON_Delete(json);
    if (text == nullptr) return ESP_ERR_NO_MEM;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(text);
    frame.len = std::strlen(text);
    const esp_err_t result = httpd_ws_send_frame(req, &frame);
    cJSON_free(text);
    return result;
}

const char* JsonString(cJSON* json, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

esp_err_t StartUpload(httpd_req_t* req, UploadSession* session, cJSON* json) {
    FileOperationGuard guard;
    if (s_storage_failed.load()) return RejectUpload(req, "内存卡状态异常，请重启设备后再试");
    if (s_http_stopping.load()) return ESP_FAIL;
    if (s_upload_busy.load()) return RejectUpload(req, "已有上传在进行，请稍后重试");
    const char* id = JsonString(json, "id");
    if (!*id || std::strlen(id) >= sizeof(s_upload.id)) return RejectUpload(req, "上传任务标识无效");
    for (const char* p = id; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == '-')) return RejectUpload(req, "上传任务标识无效");
    }
    if (std::strcmp(UploadSnapshot().id, id) == 0) return RejectUpload(req, "此上传任务已处理，请刷新目录确认结果");
    const char* encoding = JsonString(json, "encoding");
    if (*encoding && std::strcmp(encoding, "identity") && std::strcmp(encoding, "zlib-stream-v1")) {
        return RejectUpload(req, "不支持此上传编码，请刷新页面重试");
    }
    session->zlib_stream = std::strcmp(encoding, "zlib-stream-v1") == 0;
    if (session->inflater.state) inflateEnd(&session->inflater);
    session->inflater = {};
    session->stream_end = false;
    const char* web = JsonString(json, "path");
    const char* name = JsonString(json, "name");
    char dir[kFsPathCapacity];
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(json, "size");
    if (!SafeName(name) || std::strlen(web) + std::strlen(name) + (std::strcmp(web, "/") ? 1 : 0) >= kWebPathCapacity ||
        !WebToFs(web, dir, sizeof(dir)) || !cJSON_IsNumber(size) || !(size->valuedouble >= 0) ||
        size->valuedouble > static_cast<double>(static_cast<FSIZE_t>(-1)) || size->valuedouble > 9007199254740991.0) {
        return RejectUpload(req, "文件名、路径或文件大小无效");
    }
    const uint64_t expected = static_cast<uint64_t>(size->valuedouble);
    if (static_cast<double>(expected) != size->valuedouble) return RejectUpload(req, "文件大小无效");
    if (!SdCardManager::GetInstance().IsMounted()) return RejectUpload(req, "未检测到 SD 卡");
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes) != ESP_OK) return RejectUpload(req, "无法读取 SD 卡容量");
    if (expected > free_bytes) return RejectUpload(req, "SD 卡空间不足，原文件保留");
    if (!RecoverUploads(dir)) return RejectUpload(req, "目录读取或上传恢复失败");
    session->replace = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "replace"));
    std::snprintf(session->path, sizeof(session->path), "%s/%s", dir, name);
    std::snprintf(session->temp, sizeof(session->temp), "%s/.cz-%s.part", dir, name);
    std::snprintf(session->backup, sizeof(session->backup), "%s/.cz-%s.bak", dir, name);
    struct stat st;
    const int rc = stat(session->path, &st);
    if (rc == 0 && (S_ISDIR(st.st_mode) || !session->replace)) return RejectUpload(req, "已存在同名项目，请重新选择处理方式");
    if (rc != 0 && errno != ENOENT) return RejectUpload(req, "无法检查目标文件");
    // 与退出热点串行确认归属，避免开始写入后遗漏关闭清理。
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    if (s_http_stopping.load()) { xSemaphoreGive(s_upload_lock); return ESP_FAIL; }
    session->file = std::fopen(session->temp, "wb");
    if (session->file == nullptr) { xSemaphoreGive(s_upload_lock); return RejectUpload(req, "无法创建临时文件，原文件保留"); }
    s_upload = UploadState{};
    std::snprintf(s_upload.id, sizeof(s_upload.id), "%s", id);
    s_upload.fd = session->fd;
    s_upload.total = expected;
    s_upload.phase = UploadPhase::Receiving;
    s_upload_target = session->path;
    s_upload_busy.store(true);
    xSemaphoreGive(s_upload_lock);
    session->wire_bytes = 0;
    return PushUpload(req, "ready");
}

esp_err_t FinishUpload(httpd_req_t* req, UploadSession* session) {
    FileOperationGuard guard;
    auto snapshot = UploadSnapshot();
    if (snapshot.received != snapshot.total || (session->zlib_stream && !session->stream_end)) {
        ReleaseUpload(session, "上传不完整，原文件保留");
        return PushUpload(req);
    }
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    const bool cancelled = s_upload.cancel || s_http_stopping.load();
    if (!cancelled) s_upload.phase = UploadPhase::Saving;
    xSemaphoreGive(s_upload_lock);
    if (cancelled) { ReleaseUpload(session, "上传已取消，原文件保留", true); return PushUpload(req); }
    const char* error = nullptr;
    if (PushUpload(req) != ESP_OK) return ESP_FAIL;
    const int flush_result = std::fflush(session->file);
    const int flush_error = errno;
    const int sync_result = flush_result == 0 ? fsync(fileno(session->file)) : -1;
    const int sync_error = errno;
    if (flush_result != 0 || sync_result != 0) UploadWriteError(flush_result != 0 ? flush_error : sync_error, error);
    const int close_result = std::fclose(session->file);
    const int close_error = errno;
    session->file = nullptr;
    if (close_result != 0 && error == nullptr) UploadWriteError(close_error, error);
    // 提交与退出请求互斥，旧文件只在同步、关闭成功后进入备份提交。
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    if (error == nullptr && (s_upload.cancel || s_http_stopping.load())) error = "上传已取消，原文件保留";
    if (error == nullptr) {
        struct stat st;
        const int rc = stat(session->path, &st);
        const bool exists = rc == 0;
        if (exists && (!session->replace || S_ISDIR(st.st_mode))) error = "同名项目已变化，请重新上传";
        else if (!exists && errno != ENOENT) error = "无法检查目标文件，原文件保留";
        else if (exists && rename(session->path, session->backup) != 0) error = "无法保护原文件，已取消覆盖";
        else if (rename(session->temp, session->path) != 0) {
            error = "提交失败，原文件保留";
            if (exists && rename(session->backup, session->path) != 0) error = "提交失败，原文件备份已保留，恢复 SD 卡后刷新目录重试";
        } else if (exists && unlink(session->backup) != 0) ESP_LOGW(TAG, "Uploaded file committed; backup cleanup deferred");
    }
    if (error != nullptr) unlink(session->temp);
    s_upload.phase = error ? UploadPhase::Failed : UploadPhase::Done;
    if (error) std::snprintf(s_upload.error, sizeof(s_upload.error), "%s", error);
    s_upload_target = nullptr;
    s_upload_busy.store(false);
    xSemaphoreGive(s_upload_lock);
    if (error == nullptr) reader::InvalidateBookLibraryCache();
    const esp_err_t pushed = PushUpload(req);
    return pushed;
}

esp_err_t ProcessUploadRequest(httpd_req_t* req) {
    if (s_http_stopping.load()) return ESP_FAIL;
    auto* session = static_cast<UploadSession*>(req->sess_ctx);
    if (session == nullptr) return ESP_FAIL;
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || !frame.final || frame.len > kUploadChunk ||
        (frame.type == HTTPD_WS_TYPE_TEXT && frame.len > 2 * kWebPathCapacity + 256) ||
        (frame.type >= HTTPD_WS_TYPE_CLOSE && frame.len > 125)) return ESP_FAIL;
    uint8_t* buffer = session->buffer;
    frame.payload = buffer;
    if (frame.len && httpd_ws_recv_frame(req, &frame, kUploadChunk) != ESP_OK) return ESP_FAIL;
    if (frame.type == HTTPD_WS_TYPE_PONG) {
        static uint8_t pong[] = "{\"event\":\"pong\"}";
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = pong;
        frame.len = sizeof(pong) - 1;
        return httpd_ws_send_frame(req, &frame);
    }
    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        session->wire_bytes += frame.len;
        if (!OwnsUpload(session)) return ESP_FAIL;
        int result = Z_DATA_ERROR;
        if (frame.len) result = session->zlib_stream ? InflateUploadData(session, buffer, frame.len) :
            WriteUploadData(session, buffer, frame.len);
        if (result != Z_OK) {
            const char* error = "压缩流或数据长度错误，原文件保留";
            if (result == Z_MEM_ERROR || result == Z_ERRNO) UploadWriteError(result == Z_MEM_ERROR ? ENOMEM : errno, error);
            const bool cancelled = UploadSnapshot().cancel || s_http_stopping.load();
            ReleaseUpload(session, error, cancelled);
            PushUpload(req);
            return ESP_FAIL;
        }
        if (session->wire_bytes % kUploadProgressBytes < frame.len) return PushUpload(req);
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || std::memchr(buffer, 0, frame.len)) return ESP_FAIL;
    buffer[frame.len] = 0;
    cJSON* json = cJSON_ParseWithLengthOpts(reinterpret_cast<char*>(buffer), frame.len + 1, nullptr, true);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return ESP_FAIL; }
    const char* op = JsonString(json, "op");
    esp_err_t result;
    if (std::strcmp(op, "start") == 0) result = StartUpload(req, session, json);
    else if (!OwnsUpload(session) || std::strcmp(JsonString(json, "id"), UploadSnapshot().id) != 0) result = RejectUpload(req, "未找到此连接的上传任务");
    else if (std::strcmp(op, "ping") == 0) {
        frame.type = HTTPD_WS_TYPE_PING;
        frame.len = 0;
        result = httpd_ws_send_frame(req, &frame);
    } else if (std::strcmp(op, "cancel") == 0) {
        ReleaseUpload(session, "上传已取消，原文件保留", true);
        result = PushUpload(req);
    } else if (std::strcmp(op, "finish") == 0) result = FinishUpload(req, session);
    else result = RejectUpload(req, "上传命令无效");
    cJSON_Delete(json);
    return result;
}

esp_err_t HandleUploadSocket(httpd_req_t* req) {
    if (s_http_stopping.load()) return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    if (httpd_ws_get_fd_info(req->handle, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return ESP_FAIL;
    auto* session = static_cast<UploadSession*>(req->sess_ctx);
    if (session == nullptr) {
        const int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
            ESP_LOGE(TAG, "upload TCP_NODELAY errno=%d", errno);
            return ESP_FAIL;
        }
        if (xSemaphoreTake(s_upload_slots, 0) != pdTRUE) return ESP_ERR_NO_MEM;
        void* memory = heap_caps_malloc(sizeof(UploadSession), kUploadMemoryCaps);
        if (memory == nullptr) { xSemaphoreGive(s_upload_slots); return ESP_ERR_NO_MEM; }
        session = new (memory) UploadSession;
        session->buffer = static_cast<uint8_t*>(heap_caps_malloc(kUploadChunk + 1, kUploadMemoryCaps));
        if (session->buffer == nullptr) { session->~UploadSession(); heap_caps_free(session); xSemaphoreGive(s_upload_slots); return ESP_ERR_NO_MEM; }
        session->fd = fd;
        req->sess_ctx = session;
        req->free_ctx = FreeUploadSession;
    }
    return s_upload_worker.Submit(req, ProcessUploadRequest);
}

void StopUpload() {
    s_http_stopping.store(true);
    if (s_upload_lock == nullptr) return;
    xSemaphoreTake(s_upload_lock, portMAX_DELAY);
    const bool uploading = s_upload_busy.load();
    if (uploading) {
        s_upload.cancel = true;
        shutdown(s_upload.fd, SHUT_RDWR);
    }
    xSemaphoreGive(s_upload_lock);
}

esp_err_t HandleMkdir(httpd_req_t* req) {
    char body[2 * kEncodedPathCapacity + 64];
    char web[kWebPathCapacity];
    char name[kNameCapacity];
    char parent[kFsPathCapacity];
    char path[kFsPathCapacity];
    if (!ReadForm(req, body, sizeof(body)) || !FormValue(body, "path", web, sizeof(web)) ||
        !FormValue(body, "name", name, sizeof(name)) || !SafeName(name) ||
        !WebToFs(web, parent, sizeof(parent)) ||
        std::snprintf(path, sizeof(path), "%s/%s", parent, name) >= static_cast<int>(sizeof(path)) ||
        std::strlen(path) - std::strlen(SD_MOUNT_POINT) >= kWebPathCapacity) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (AffectsUpload(path)) return SendText(req, "409 Conflict", "此路径正在上传，请完成后重试");
    if (mkdir(path, 0775) != 0) {
        return SendText(req, "500 Internal Server Error", "创建失败");
    }
    return SendText(req, nullptr, "ok");
}

bool DeleteTree(char* path, size_t capacity, bool recursive, uint32_t* deleted) {
    const size_t root_len = std::strlen(path);
    while (true) {
        if (s_http_stopping.load()) return false;
        struct stat st;
        if (stat(path, &st) != 0) return false;
        if (S_ISDIR(st.st_mode) && recursive) {
            DIR* dir = opendir(path);
            if (dir == nullptr) return false;
            bool child = false, valid = true;
            const size_t len = std::strlen(path);
            errno = 0;
            while (dirent* entry = readdir(dir)) {
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
                const size_t n = std::strlen(entry->d_name);
                if (len + 1 + n >= capacity) { valid = false; break; }
                path[len] = '/'; std::memcpy(path + len + 1, entry->d_name, n + 1);
                child = true; break;
            }
            if (!child && errno != 0) valid = false;
            if (closedir(dir) != 0) valid = false;
            if (!valid) return false;
            if (child) continue;
        }
        if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) return false;
        ++*deleted;
        if (std::strlen(path) == root_len) return true;
        *std::strrchr(path, '/') = '\0';
        if ((*deleted % 16) == 0) vTaskDelay(1);
    }
}

esp_err_t HandleDelete(httpd_req_t* req) {
    char body[2 * kEncodedPathCapacity + 64], web[kWebPathCapacity], path[kFsPathCapacity], recursive[4];
    if (!ReadForm(req, body, sizeof(body)) || !FormValue(body, "path", web, sizeof(web)) ||
        !WebToFs(web, path, sizeof(path)) || std::strcmp(path, SD_MOUNT_POINT) == 0) {
        return SendText(req, "400 Bad Request", "路径无效或不允许删除根目录");
    }
    if (!SdCardManager::GetInstance().IsMounted()) return SendText(req, "503 Service Unavailable", "未检测到 SD 卡");
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (AffectsUpload(path)) return SendText(req, "409 Conflict", "此路径包含正在上传的文件，请完成后重试");
    const bool recurse = FormValue(body, "recursive", recursive, sizeof(recursive)) && std::strcmp(recursive, "1") == 0;
    uint32_t deleted = 0;
    const bool ok = DeleteTree(path, sizeof(path), recurse, &deleted);
    if (deleted > 0) reader::InvalidateBookLibraryCache();
    char json[256];
    std::snprintf(json, sizeof(json), "{\"deleted\":%lu,\"error\":\"%s\"}", static_cast<unsigned long>(deleted),
                  ok ? "" : "部分项目未删除，可能是文件使用中、路径过长或 SD 卡异常；已删除内容无法恢复，请刷新确认");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, json);
}

esp_err_t HandleRename(httpd_req_t* req) {
    char body[2 * kEncodedPathCapacity + 64];
    char web[kWebPathCapacity];
    char name[kNameCapacity];
    char from[kFsPathCapacity];
    char to[kFsPathCapacity];
    if (!ReadForm(req, body, sizeof(body)) || !FormValue(body, "path", web, sizeof(web)) ||
        !FormValue(body, "name", name, sizeof(name)) || !SafeName(name) ||
        !WebToFs(web, from, sizeof(from)) || std::strcmp(from, SD_MOUNT_POINT) == 0) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    char parent[kFsPathCapacity];
    std::snprintf(parent, sizeof(parent), "%s", from);
    char* slash = std::strrchr(parent, '/');
    if (slash == nullptr) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    *slash = '\0';
    if (std::snprintf(to, sizeof(to), "%s/%s", parent, name) >= static_cast<int>(sizeof(to))) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (AffectsUpload(from) || AffectsUpload(to)) return SendText(req, "409 Conflict", "此路径正在上传，请完成后重试");
    if (std::strlen(to) - std::strlen(SD_MOUNT_POINT) >= kWebPathCapacity || !RecoverUploads(parent)) {
        return SendText(req, "400 Bad Request", "路径过长或目录暂不可用");
    }
    struct stat st;
    if (stat(to, &st) == 0) return SendText(req, "409 Conflict", "已存在同名项目");
    if (errno != ENOENT) return SendText(req, "500 Internal Server Error", "无法检查目标项目");
    if (rename(from, to) != 0) {
        return SendText(req, "500 Internal Server Error", "改名失败");
    }
    reader::InvalidateBookLibraryCache();
    return SendText(req, nullptr, "ok");
}

esp_err_t HandleMove(httpd_req_t* req) {
    char body[2 * kEncodedPathCapacity + 64];
    char web[kWebPathCapacity];
    char to_web[kWebPathCapacity];
    char from[kFsPathCapacity];
    char dest_dir[kFsPathCapacity];
    char dest[kFsPathCapacity];
    if (!ReadForm(req, body, sizeof(body)) || !FormValue(body, "path", web, sizeof(web)) ||
        !FormValue(body, "to", to_web, sizeof(to_web)) || !WebToFs(web, from, sizeof(from)) ||
        !WebToFs(to_web, dest_dir, sizeof(dest_dir)) || std::strcmp(from, SD_MOUNT_POINT) == 0) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    const size_t from_len = std::strlen(from);
    if (strncasecmp(dest_dir, from, from_len) == 0 && (dest_dir[from_len] == '/' || dest_dir[from_len] == '\0')) {
        return SendText(req, "400 Bad Request", "不能移动到自身或子目录");
    }
    const char* base = std::strrchr(from, '/');
    base = base != nullptr ? base + 1 : from;
    if (!SafeName(base) ||
        std::snprintf(dest, sizeof(dest), "%s/%s", dest_dir, base) >= static_cast<int>(sizeof(dest))) {
        return SendText(req, "400 Bad Request", "路径无效");
    }
    FileOperationGuard guard;
    if (s_http_stopping.load()) return ESP_FAIL;
    if (AffectsUpload(from) || AffectsUpload(dest)) return SendText(req, "409 Conflict", "此路径正在上传，请完成后重试");
    if (std::strlen(dest) - std::strlen(SD_MOUNT_POINT) >= kWebPathCapacity || !RecoverUploads(dest_dir)) {
        return SendText(req, "400 Bad Request", "路径过长或目标目录暂不可用");
    }
    struct stat st;
    if (stat(dest, &st) == 0) return SendText(req, "409 Conflict", "目标已存在同名项目");
    if (errno != ENOENT) return SendText(req, "500 Internal Server Error", "无法检查目标项目");
    if (rename(from, dest) != 0) {
        return SendText(req, "500 Internal Server Error", "移动失败");
    }
    reader::InvalidateBookLibraryCache();
    return SendText(req, nullptr, "ok");
}

struct RequestRoute {
    const char* uri; // 已注册路径，不含用户查询参数
    httpd_method_t method; // HTTP 方法
    esp_err_t (*handler)(httpd_req_t*); // 原业务处理器
};

esp_err_t ProcessFileRequest(httpd_req_t* req) {
    const auto* route = static_cast<const RequestRoute*>(req->user_ctx);
    if (s_http_stopping.load()) return SendText(req, "503 Service Unavailable", "热点正在关闭");
    if (s_upload_busy.load() && route->handler == HandleDownload) {
        return SendText(req, "503 Service Unavailable", "上传期间暂不支持下载，请完成后重试");
    }
    return route->handler(req);
}

esp_err_t HandleRequest(httpd_req_t* req) {
    if (s_http_stopping.load()) return ESP_FAIL;
    const esp_err_t err = s_file_worker.Submit(req, ProcessFileRequest);
    if (err != ESP_OK) return SendText(req, "503 Service Unavailable", "文件请求繁忙，请稍后重试");
    return ESP_OK;
}

bool StartHttp() {
    for (auto& fd : s_clients) fd = -1;
    if (s_upload_lock == nullptr) s_upload_lock = xSemaphoreCreateMutexStatic(&s_upload_lock_storage);
    if (s_upload_lock == nullptr) return false;
    if (!s_file_lock) s_file_lock = xSemaphoreCreateRecursiveMutexStatic(&s_file_lock_storage);
    if (!s_clients_lock) s_clients_lock = xSemaphoreCreateMutexStatic(&s_clients_lock_storage);
    if (!s_upload_slots) s_upload_slots = xSemaphoreCreateCountingStatic(ApRequestWorker::kMaxClients, ApRequestWorker::kMaxClients, &s_upload_slots_storage);
    if (!s_file_lock || !s_clients_lock || !s_upload_slots) return false;
    s_http_stopping.store(false);
    if (!s_upload_worker.Start("ap_upload") || !s_file_worker.Start("ap_files")) return false;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.max_open_sockets = ApRequestWorker::kMaxClients;
    config.open_fn = OpenClient;
    config.close_fn = CloseClient;
    config.stack_size = 16384;
    #if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    #endif
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 10;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        s_httpd = nullptr;
        return false;
    }
    static const RequestRoute routes[] = {
        {"/", HTTP_GET, HandlePage}, {"/files", HTTP_GET, HandlePage},
        {"/api/files", HTTP_GET, HandleFileList}, {"/api/storage", HTTP_GET, HandleStorage},
        {"/download", HTTP_GET, HandleDownload},
        {"/mkdir", HTTP_POST, HandleMkdir}, {"/delete", HTTP_POST, HandleDelete},
        {"/rename", HTTP_POST, HandleRename}, {"/move", HTTP_POST, HandleMove},
    };
    for (const auto& entry : routes) {
        httpd_uri_t route = {};
        route.uri = entry.uri; route.method = entry.method; route.handler = HandleRequest;
        route.user_ctx = const_cast<RequestRoute*>(&entry);
        if (httpd_register_uri_handler(s_httpd, &route) != ESP_OK) {
            return false;
        }
    }
    httpd_uri_t upload = {};
    upload.uri = "/ws/upload";
    upload.method = HTTP_GET;
    upload.handler = HandleUploadSocket;
    upload.is_websocket = true;
    // SDK 同步处理 CLOSE，避免会话在异步请求完成前被回收；PONG 仍交给上传任务。
    upload.handle_ws_control_frames = false;
    if (httpd_register_uri_handler(s_httpd, &upload) != ESP_OK) {
        return false;
    }
    return true;
}

const char* StartRadio(const char* ssid) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init %s", esp_err_to_name(err));
        return Lang::Strings::CLOUD_AP_FAIL_INIT;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop %s", esp_err_to_name(err));
        return Lang::Strings::CLOUD_AP_FAIL_INIT;
    }
    if (s_ap_netif == nullptr) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == nullptr) {
            ESP_LOGE(TAG, "ap netif create failed");
            return Lang::Strings::CLOUD_AP_FAIL_INIT;
        }
    }
    esp_netif_ip_info_t ip = {};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "wifi init failed");
            return Lang::Strings::CLOUD_AP_FAIL_INIT;
        }
        s_we_inited = true;
    } else {
        WifiStation::GetInstance().PauseForLp();
        s_paused_sta = WifiStation::GetInstance().IsLpPaused();
    }

    wifi_config_t ap = {};
    std::snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", ssid);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "softap start failed");
        return Lang::Strings::CLOUD_AP_FAIL_START;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
    if (esp_wifi_set_max_tx_power(kMetalioWifiMaxTxPower) != ESP_OK) {
        return Lang::Strings::CLOUD_AP_FAIL_START;
    }
    #endif
    return nullptr;
}

}  // namespace

bool ApTransferNeedsStorageRestart() {
    return s_storage_failed.load() && !s_upload_busy.load();
}

static const char* TransferWifiUnavailableReason() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual != nullptr && dual->GetNetworkType() == NetworkType::CELLULAR) {
        return Lang::Strings::CLOUD_CELLULAR_NEED_WIFI;
    }
    return WifiStation::GetInstance().IsUserEnabled() ? nullptr : Lang::Strings::WIFI_DISABLED_RETRY;
}

bool WifiTransferStart(ApTransferInfo* info) {
    LockRadio();
    if (!info || s_standby_hold.load() || (s_running && !s_station_service)) {
        UnlockRadio();
        return false;
    }
    *info = {};
    if (const char* reason = TransferWifiUnavailableReason()) {
        info->fail = reason;
        UnlockRadio();
        return false;
    }
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    wifi_ap_record_t ap{};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
        !ip.ip.addr || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        info->fail = Lang::Strings::CLOUD_WIFI_FAIL;
        UnlockRadio();
        return false;
    }
    std::snprintf(info->ssid, sizeof(info->ssid), "%.*s", 32, reinterpret_cast<char*>(ap.ssid));
    std::snprintf(info->url, sizeof(info->url), "http://" IPSTR "/", IP2STR(&ip.ip));
    s_station_service = true;
    if (!s_running && !StartHttp()) {
        info->fail = Lang::Strings::CLOUD_AP_FAIL_HTTP;
        ApTransferStop();
        UnlockRadio();
        return false;
    }
    HoldPower();
    s_running = true;
    info->ok = true;
    UnlockRadio();
    return true;
}

bool ApTransferStart(ApTransferInfo* info) {
    LockRadio();
    if (info != nullptr) {
        std::memset(info, 0, sizeof(*info));
        FillSsid(info->ssid, sizeof(info->ssid));
        std::snprintf(info->url, sizeof(info->url), "%s", kUrl);
    }
    const char* unavailable = TransferWifiUnavailableReason();
    if (s_standby_hold.load() || unavailable != nullptr) {
        if (info != nullptr) info->fail = unavailable;
        UnlockRadio();
        return false;
    }
    if (s_running) {
        if (info != nullptr) {
            info->ok = true;
        }
        UnlockRadio();
        return true;
    }
    char ssid[33];
    FillSsid(ssid, sizeof(ssid));
    const char* fail = StartRadio(ssid);
    if (fail == nullptr && !StartHttp()) {
        fail = Lang::Strings::CLOUD_AP_FAIL_HTTP;
    }
    if (fail != nullptr) {
        ApTransferStop();
        if (info != nullptr) {
            info->fail = fail;
        }
        UnlockRadio();
        return false;
    }
    HoldPower();
    s_running = true;
    if (info != nullptr) {
        info->ok = true;
    }
    UnlockRadio();
    return true;
}

void ApTransferStop(bool resume_sta) {
    LockRadio();
    StopUpload();
    s_upload_worker.CancelRequests();
    s_file_worker.CancelRequests();
    ShutdownClients();
    s_upload_worker.Drain();
    s_file_worker.Drain();
    if (s_httpd != nullptr) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    // HTTP 停止后不再产生会话回收回调，最后排空上传清理并退出任务。
    s_upload_worker.Stop();
    s_file_worker.Stop();
    if (!s_station_service && (s_running || s_paused_sta || s_we_inited)) {
        esp_wifi_stop();
        if (s_we_inited) {
            esp_wifi_deinit();
            s_we_inited = false;
        } else if (s_paused_sta) {
            // 切到 U 盘只关热点，不把 STA 拉起来
            esp_wifi_set_mode(WIFI_MODE_STA);
            if (resume_sta && WifiStation::GetInstance().IsUserEnabled()) {
                auto& sta = WifiStation::GetInstance();
                if (sta.IsLpPaused()) {
                    sta.ResumeFromLp();
                } else {
                    esp_wifi_start();
                }
                s_paused_sta = false;
            }
        }
    }
    s_running = false;
    s_station_service = false;
    s_upload_busy = false;
    ReleasePower();
    UnlockRadio();
}

void ApTransferRequestStop(bool standby) {
    if (standby) s_standby_hold.store(true);
    s_http_stopping.store(true);
}

void ApTransferAllowStart() {
    s_standby_hold.store(false);
}
