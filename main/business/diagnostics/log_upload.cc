#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "business/conversation/activation_polling.h"
#include "log_upload.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/sd_log_writer.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

static const char* TAG = "LogUpload";
static constexpr const char* kBoundary = "----ESP32_DEVICE_LOG_BOUNDARY";
static constexpr int64_t kUploadBudgetUs = 120LL * 1000000;
static LogUploadSessions s_sessions;

static bool Abandoned(const LogUploadSession& s, int64_t deadline) {
    return s.cancel.load() || esp_timer_get_time() >= deadline;
}

static LogUploadResult UploadFailure(LogUploadSession& session, const char* stage, int code = 0) {
    if (session.cancel.load()) {
        return LogUploadResult::Cancelled;
    }
    ESP_LOGE(TAG, "operation=upload stage=%s code=%d", stage, code);
    return LogUploadResult::Failed;
}

// ESP-IDF HTTP client is private to this worker: no concurrent Close/Write, gate
// reset, borrowed Http*, or abort task. Network calls use a 60-second I/O timeout;
// cancellation is cooperative and does not claim to interrupt DNS immediately.
struct HttpOwner {
    esp_http_client_handle_t handle = nullptr;

    ~HttpOwner() {
        if (handle) {
            esp_http_client_cleanup(handle);
        }
    }
};

static bool WriteAll(esp_http_client_handle_t http, const char* data, size_t size, LogUploadSession& session,
                     int64_t deadline) {
    size_t offset = 0;
    while (offset < size) {
        if (Abandoned(session, deadline)) {
            return false;
        }
        int n = esp_http_client_write(http, data + offset, static_cast<int>(size - offset));
        if (n <= 0) {
            return false;
        }
        offset += n;
    }
    return true;
}

static bool ReadResponse(esp_http_client_handle_t http, char* response, size_t capacity, LogUploadSession& session,
                         int64_t deadline) {
    size_t used = 0;
    while (used + 1 < capacity) {
        if (Abandoned(session, deadline)) {
            return false;
        }
        // fetch_headers can parse the complete response but still retain unread
        // body bytes. Always consume Read before examining parser completeness.
        int n = esp_http_client_read(http, response + used, capacity - 1 - used);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            response[used] = '\0';
            return used > 0 && esp_http_client_is_complete_data_received(http);
        }
        used += n;
    }
    if (Abandoned(session, deadline)) {
        return false;
    }
    char extra;
    if (esp_http_client_read(http, &extra, 1) != 0) {
        return false;  // reject oversized body
    }
    response[used] = '\0';
    return esp_http_client_is_complete_data_received(http);
}

static LogUploadResult SendSnapshot(LogUploadSession& session, const SdLogSnapshot& snapshot, bool crash, size_t sent,
                                    size_t total) {
    const unsigned previous = session.jobs.load();
    session.jobs.store((previous & 0xffffU) | ((crash ? 2U : 1U) << 16));
    std::string mac = ApiDeviceId();
    for (char& ch : mac) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    std::string url = ApiDeviceLogsUploadUrl(mac);
    if (url.empty()) {
        return UploadFailure(session, "configuration");
    }
    const int64_t deadline = esp_timer_get_time() + kUploadBudgetUs;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 60000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    HttpOwner http;
    http.handle = esp_http_client_init(&config);
    if (!http.handle) {
        return LogUploadResult::NoMemory;
    }
    std::string ctype = std::string("multipart/form-data; boundary=") + kBoundary;
    if (esp_http_client_set_header(http.handle, "Device-Id", ApiDeviceId().c_str()) != ESP_OK ||
        esp_http_client_set_header(http.handle, "Accept", "*/*") != ESP_OK ||
        esp_http_client_set_header(http.handle, "Connection", "close") != ESP_OK ||
        esp_http_client_set_header(http.handle, "Content-Type", ctype.c_str()) != ESP_OK) {
        return UploadFailure(session, "headers");
    }
    char header[220], footer[64];
    int hn = snprintf(header, sizeof(header),
                      "--%s\r\nContent-Disposition: form-data; name=\"log\"; filename=\"%s\"\r\n"
                      "Content-Type: %s\r\n\r\n",
                      kBoundary, crash ? "metalio_coredump.elf" : "metalio_logs.txt",
                      crash ? "application/octet-stream" : "text/plain");
    int fn = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", kBoundary);
    if (hn <= 0 || size_t(hn) >= sizeof(header) || fn <= 0 || size_t(fn) >= sizeof(footer)) {
        return UploadFailure(session, "multipart");
    }
    if (Abandoned(session, deadline)) {
        return LogUploadResult::Cancelled;
    }
    const auto open_result = esp_http_client_open(http.handle, int(snapshot.size + hn + fn));
    if (open_result != ESP_OK) {
        return UploadFailure(session, "open", static_cast<int>(open_result));
    }
    if (!WriteAll(http.handle, header, hn, session, deadline)) {
        return UploadFailure(session, "write_header");
    }
    session.phase.store(LogUploadPhase::Sending);
    for (size_t offset = 0; offset < snapshot.size;) {
        size_t size = std::min(snapshot.size - offset, size_t(1024));
        if (!WriteAll(http.handle, reinterpret_cast<const char*>(snapshot.data + offset), size, session, deadline)) {
            return UploadFailure(session, "write_body");
        }
        offset += size;
        session.percent.store(std::min(99, int((sent + offset) * 100ULL / total)));
    }
    if (!WriteAll(http.handle, footer, fn, session, deadline)) {
        return UploadFailure(session, "write_footer");
    }
    if (Abandoned(session, deadline)) {
        return UploadFailure(session, "response_budget");
    }
    const auto header_result = esp_http_client_fetch_headers(http.handle);
    if (header_result < 0) {
        return UploadFailure(session, "response_header", static_cast<int>(header_result));
    }
    int status = esp_http_client_get_status_code(http.handle);
    if (status < 200 || status >= 300) {
        return UploadFailure(session, "response_status", status);
    }
    char response[512];
    if (!ReadResponse(http.handle, response, sizeof(response), session, deadline)) {
        return UploadFailure(session, "response_body");
    }
    cJSON* root = cJSON_ParseWithOpts(response, nullptr, true);
    if (!root) {
        return UploadFailure(session, "response_json");
    }
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = (cJSON_IsNumber(code) && code->valuedouble == 0) ||
              (cJSON_IsString(code) && code->valuestring && strcmp(code->valuestring, "0") == 0);
    cJSON_Delete(root);
    if (ok) {
        session.jobs.fetch_add(1U << 8);
    }
    return ok ? LogUploadResult::Ok : UploadFailure(session, "response_code");
}

struct ActivationHold {
    ActivationHold() {
        ActivationPolling::GetInstance().Pause();
    }

    ~ActivationHold() {
        ActivationPolling::GetInstance().Resume();
    }
};

static LogUploadResult RunUpload(LogUploadSession& session) {
    if (session.cancel.load()) {
        return LogUploadResult::Cancelled;
    }
    PowerNeedHold power(PowerNeed::OtaDownload);
    ActivationHold activation;
    int64_t wait_deadline = esp_timer_get_time() + 30LL * 1000000;
    while (ActivationPolling::GetInstance().IsBusy()) {
        if (Abandoned(session, wait_deadline)) {
            return LogUploadResult::NetFailed;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (session.cancel.load()) {
        return LogUploadResult::Cancelled;
    }
    SdLogSnapshot snapshot;
    switch (SdLogWriter_Snapshot(snapshot)) {
        case SdLogSnapshotResult::NoFile:
            break;
        case SdLogSnapshotResult::Unavailable:
            return LogUploadResult::StorageUnavailable;
        case SdLogSnapshotResult::NoMemory:
            return LogUploadResult::NoMemory;
        case SdLogSnapshotResult::IoError:
            return UploadFailure(session, "snapshot");
        case SdLogSnapshotResult::Ok:
            break;
    }
    SdLogSnapshot crash;
    auto crash_result = SdLogWriter_CrashSnapshot(crash);
    if (crash_result == SdLogSnapshotResult::Unavailable) {
        return LogUploadResult::StorageUnavailable;
    }
    if (crash_result == SdLogSnapshotResult::NoMemory) {
        return LogUploadResult::NoMemory;
    }
    if (crash_result == SdLogSnapshotResult::IoError) {
        return UploadFailure(session, "crash_snapshot");
    }
    if (!snapshot.size && !crash.size) {
        return LogUploadResult::NoFile;
    }
    session.jobs.store(unsigned(snapshot.size > 0) + unsigned(crash.size > 0));
    session.phase.store(LogUploadPhase::WaitingNetwork);
    bool ready = false;
    wait_deadline = esp_timer_get_time() + 30LL * 1000000;
    while (!Abandoned(session, wait_deadline)) {
        if (Board::GetInstance().IsWifiConfigMode()) {
            break;
        }
        if (Board::GetInstance().EnsureNetworkReady(200)) {
            ready = true;
            break;
        }
        if (Board::GetInstance().GetNetworkErrorText(nullptr)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (session.cancel.load()) {
        return LogUploadResult::Cancelled;
    }
    if (!ready) {
        return LogUploadResult::NetFailed;
    }
    if (snapshot.size) {
        auto result = SendSnapshot(session, snapshot, false, 0, snapshot.size + crash.size);
        if (result != LogUploadResult::Ok) {
            return result;
        }
    }
    if (crash.size) {
        return SendSnapshot(session, crash, true, snapshot.size, snapshot.size + crash.size);
    }
    return LogUploadResult::Ok;
}

static void Worker(const LogUploadSessionPtr& session) {
    LogUploadResult result = LogUploadResult::NoMemory;
    try {
        result = RunUpload(*session);
    } catch (const std::bad_alloc&) {
        result = LogUploadResult::NoMemory;
    }
    // RunUpload has already destroyed HTTP, snapshot and all guards.
    const bool cancelled = session->cancel.load();
    if (!cancelled && result == LogUploadResult::NoMemory) {
        ESP_LOGE(TAG, "operation=upload phase=%u result=%u", unsigned(session->phase.load()), unsigned(result));
    } else if (!cancelled &&
               (result == LogUploadResult::NetFailed || result == LogUploadResult::StorageUnavailable)) {
        ESP_LOGW(TAG, "operation=upload phase=%u result=%u", unsigned(session->phase.load()), unsigned(result));
    } else {
        ESP_LOGI(TAG, "operation=upload result=%u cancelled=%d", unsigned(result), cancelled);
    }
    session->Finish(result);
}

LogUploadSessionPtr LogUploadStart() {
    auto current = s_sessions.Current();
    if (current && current->running.load(std::memory_order_acquire)) {
        return current;
    }
    LogUploadSessionPtr session(new (std::nothrow) LogUploadSession);
    if (!session) {
        ESP_LOGE(TAG, "operation=start stage=session result=no_memory");
        return {};
    }
    if (!s_sessions.Begin(session)) {
        return s_sessions.Current();
    }
    bool scheduled = false;
    try {
        scheduled = TaskExecutor::Schedule([session] { Worker(session); }, TaskDomain::Transfer);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=start stage=submit result=no_memory");
    }
    if (!scheduled) {
        session->Finish(LogUploadResult::NoMemory);
        ESP_LOGE(TAG, "operation=start stage=worker result=no_memory");
    }
    return session;
}

void LogUploadCancel() {
    s_sessions.Cancel();
}
