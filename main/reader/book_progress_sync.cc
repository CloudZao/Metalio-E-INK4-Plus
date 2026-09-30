#include "book_progress_sync.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "book_library.h"
#include "book_library_warmup.h"
#include "book_progress_cache.h"
#include "book_session.h"
#include "power_policy.h"
#include "reader_types.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reader {
namespace book_progress_sync {
namespace {

constexpr const char* TAG = "BookProgSync";
constexpr int kHttpTimeoutMs = 30000;
constexpr uint32_t kWorkerStack = 10 * 1024;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 2;
constexpr size_t kReportMaxBooks = 500;
constexpr TickType_t kRetryGapTicks = pdMS_TO_TICKS(20);
/** 等待 warmup：200ms × 3000 ≈ 10min（大书库 Peek） */
constexpr TickType_t kWaitSliceTicks = pdMS_TO_TICKS(200);
constexpr int kWaitMaxSlices = 3000;

std::atomic<bool> s_running{false};
std::atomic<uint8_t> s_phase{static_cast<uint8_t>(Phase::kIdle)};

void SetPhase(Phase phase) {
    s_phase.store(static_cast<uint8_t>(phase), std::memory_order_release);
}

const char* BasenameOf(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return "";
    }
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

size_t DeduplicateBooksByBasename(std::vector<BookInfo>& books) {
    if (books.size() <= 1) {
        return 0;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(books.size());

    size_t write = 0;
    size_t dropped = 0;
    for (size_t i = 0; i < books.size(); ++i) {
        const char* name = BasenameOf(books[i].path.c_str());
        if (name[0] == '\0') {
            ++dropped;
            continue;
        }
        if (!seen.emplace(name).second) {
            ESP_LOGW(TAG, "同步去重: 丢掉重名 name=%s path=%s", name, books[i].path.c_str());
            ++dropped;
            continue;
        }
        if (write != i) {
            books[write] = std::move(books[i]);
        }
        ++write;
    }
    books.resize(write);
    if (dropped > 0) {
        ESP_LOGI(TAG, "同步去重完成: 丢掉 %u 本重名, 上报 %u 本",
                 static_cast<unsigned>(dropped), static_cast<unsigned>(books.size()));
    }
    return dropped;
}

std::string BuildReportBodyFromCache(const std::vector<BookInfo>& books, size_t begin,
                                     size_t end) {
    if (end > books.size()) {
        end = books.size();
    }
    if (begin > end) {
        begin = end;
    }

    std::vector<BookInfo> slice(books.begin() + static_cast<std::ptrdiff_t>(begin),
                                books.begin() + static_cast<std::ptrdiff_t>(end));
    std::vector<BookSession::ProgressPeek> peeks;
    const size_t hits = book_progress_cache::FillPeeksForBooks(slice, peeks);
    ESP_LOGI(TAG, "sync body from cache hits=%u/%u", static_cast<unsigned>(hits),
             static_cast<unsigned>(slice.size()));

    cJSON* arr = cJSON_CreateArray();
    if (arr == nullptr) {
        return {};
    }

    for (size_t i = 0; i < slice.size(); ++i) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;
        }
        const char* name = BasenameOf(slice[i].path.c_str());
        if (name[0] == '\0') {
            cJSON_Delete(item);
            continue;
        }
        if (cJSON_AddStringToObject(item, "name", name) == nullptr) {
            cJSON_Delete(item);
            continue;
        }

        const auto& peek = peeks[i];
        const double progress =
            peek.progress_x10 >= 0 ? static_cast<double>(peek.progress_x10) / 10.0 : 0.0;
        if (cJSON_AddNumberToObject(item, "progress", progress) == nullptr ||
            cJSON_AddNumberToObject(item, "readingSeconds",
                                    static_cast<double>(peek.reading_seconds)) == nullptr ||
            cJSON_AddNumberToObject(item, "todayReadingSeconds",
                                    static_cast<double>(peek.daily_seconds)) == nullptr) {
            cJSON_Delete(item);
            continue;
        }

        if (!cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
        }
    }

    ESP_LOGI(TAG, "payload books=%d range=[%u,%u)", cJSON_GetArraySize(arr),
             static_cast<unsigned>(begin), static_cast<unsigned>(end));

    char* printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (printed == nullptr) {
        return {};
    }
    std::string body(printed);
    cJSON_free(printed);
    return body;
}

bool PostProgressReport(const std::string& body, std::string& err_out) {
    err_out.clear();
    if (body.empty()) {
        err_out = Lang::Strings::BOOK_SYNC_EMPTY_BODY;
        return false;
    }

    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::BOOK_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::BOOK_CONN_FAIL;
        return false;
    }

    const std::string url = api::LibrarySyncUrl();
    if (url.empty()) {
        ESP_LOGI(TAG, "skip library sync: cloud endpoints blank");
        return true;
    }
    http->SetTimeout(kHttpTimeoutMs);
    api::ApplyJsonHeaders(http);
    api::LogHttpRequest(TAG, "POST", url, body);
    http->SetContent(std::string(body));
    if (!http->Open("POST", url)) {
        err_out = Lang::Strings::CLOUD_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }

    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));

    if (status < 200 || status >= 300) {
        err_out = "HTTP " + std::to_string(status);
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
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
    if (cJSON_IsObject(data)) {
        const cJSON* applied = cJSON_GetObjectItemCaseSensitive(data, "applied");
        const cJSON* snap = cJSON_GetObjectItemCaseSensitive(data, "snapshotVersion");
        const cJSON* book_count = cJSON_GetObjectItemCaseSensitive(data, "bookCount");
        const cJSON* ua = cJSON_GetObjectItemCaseSensitive(data, "userAgentId");
        const cJSON* sync_t = cJSON_GetObjectItemCaseSensitive(data, "lastSyncTime");

        char snap_buf[32] = "?";
        if (cJSON_IsString(snap) && snap->valuestring != nullptr) {
            std::snprintf(snap_buf, sizeof(snap_buf), "%s", snap->valuestring);
        } else if (cJSON_IsNumber(snap)) {
            std::snprintf(snap_buf, sizeof(snap_buf), "%d", snap->valueint);
        }

        ESP_LOGI(TAG,
                 "report ok applied=%d snapshotVersion=%s bookCount=%d userAgentId=%s "
                 "lastSyncTime=%s",
                 cJSON_IsTrue(applied) ? 1 : 0, snap_buf,
                 cJSON_IsNumber(book_count) ? book_count->valueint : -1,
                 cJSON_IsString(ua) && ua->valuestring ? ua->valuestring : "?",
                 cJSON_IsString(sync_t) && sync_t->valuestring ? sync_t->valuestring : "?");
    } else {
        ESP_LOGI(TAG, "report ok (no data object)");
    }

    cJSON_Delete(root);
    return true;
}

bool WaitWarmupReady() {
    SetPhase(Phase::kWaitCache);
    for (int i = 0; i < kWaitMaxSlices; ++i) {
        if (book_library_warmup::IsProgressCacheReady()) {
            return true;
        }
        vTaskDelay(kWaitSliceTicks);
    }
    return book_library_warmup::IsProgressCacheReady();
}

bool PostOnceFromCache() {
    std::vector<BookInfo> books;
    if (!LoadBookLibrary(kDefaultBooksDir, books)) {
        ESP_LOGW(TAG, "阅读时长同步: 无法加载书库列表");
        return false;
    }

    DeduplicateBooksByBasename(books);

    if (books.empty()) {
        SetPhase(Phase::kPost);
        std::string err;
        if (!PostProgressReport("[]", err)) {
            ESP_LOGW(TAG, "阅读时长同步失败(空书库): %s", err.c_str());
            return false;
        }
        ESP_LOGI(TAG, "阅读时长已同步: 0 本(空书库快照 [])");
        return true;
    }

    const size_t total = books.size();
    const size_t report_n = total > kReportMaxBooks ? kReportMaxBooks : total;
    if (total > kReportMaxBooks) {
        ESP_LOGW(TAG, "阅读时长同步: 本地 %u 本, 仅上报前 %u 本(上限)",
                 static_cast<unsigned>(total), static_cast<unsigned>(kReportMaxBooks));
    } else {
        ESP_LOGI(TAG, "阅读时长同步开始: %u 本(缓存组包)", static_cast<unsigned>(total));
    }

    std::string body = BuildReportBodyFromCache(books, 0, report_n);
    if (body.empty()) {
        ESP_LOGW(TAG, "阅读时长同步失败: 组包失败");
        return false;
    }

    SetPhase(Phase::kPost);
    std::string err;
    bool ok = PostProgressReport(body, err);
    if (!ok) {
        ESP_LOGW(TAG, "阅读时长同步失败: %s; 重试一次", err.c_str());
        vTaskDelay(kRetryGapTicks);
        err.clear();
        ok = PostProgressReport(body, err);
    }
    body.clear();
    body.shrink_to_fit();

    if (!ok) {
        ESP_LOGW(TAG, "阅读时长同步失败(重试后): %s", err.c_str());
        return false;
    }

    ESP_LOGI(TAG, "阅读时长已同步: %u/%u 本(单次 POST)", static_cast<unsigned>(report_n),
             static_cast<unsigned>(total));
    return true;
}

void WorkerTask(void* /*arg*/) {
    {
        ESP_LOGI(TAG, "library/sync 上报: 开始");
        if (!Board::GetInstance().IsNetworkReady()) {
            ESP_LOGW(TAG, "library/sync 上报: 跳过(网络未就绪)");
        } else if (!WaitWarmupReady()) {
            ESP_LOGW(TAG, "library/sync 上报: 跳过(进度缓存未就绪/超时)");
        } else {
            (void)PostOnceFromCache();
        }
        ESP_LOGI(TAG, "library/sync 上报: 结束");
    }

    SetPhase(Phase::kIdle);
    s_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

void RequestBootReport() {
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGD(TAG, "boot report already running");
        return;
    }
    SetPhase(Phase::kWaitCache);

    const BaseType_t ok =
        xTaskCreatePinnedToCore(WorkerTask, "book_prog_sync", kWorkerStack, nullptr,
                                kWorkerPriority, nullptr, 0);
    if (ok != pdPASS) {
        SetPhase(Phase::kIdle);
        s_running.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "xTaskCreate(book_prog_sync) failed");
    }
}

bool IsBusy() {
    return s_running.load(std::memory_order_acquire);
}

Progress GetProgress() {
    Progress p;
    p.busy = s_running.load(std::memory_order_acquire);
    p.phase = static_cast<Phase>(s_phase.load(std::memory_order_acquire));
    if (!p.busy) {
        p.phase = Phase::kIdle;
    }
    return p;
}

}  // namespace book_progress_sync
}  // namespace reader
