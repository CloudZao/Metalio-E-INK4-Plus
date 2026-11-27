#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "checklist_service.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "business/checklist/checklist_cache.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "Checklist";
static constexpr int kHttpTimeoutMs = 15000;
static constexpr int kMaxItems = CHECKLIST_CACHE_MAX_ITEMS;
static std::atomic<bool> s_busy{false}; // 包括启动同步和异步操作

static bool ParseItemsJson(const std::string& body, std::vector<ChecklistItem>& out, std::string& err) {
    out.clear();
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> document(cJSON_Parse(body.c_str()), cJSON_Delete);
    cJSON* root = document.get();
    if (root == nullptr) {
        err = LangString_TASK_PARSE_FAIL;
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            err = msg->valuestring;
        } else {
            err = LangString_TASK_API_ERROR;
        }
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        err = LangString_TASK_DATA_FORMAT_ERR;
        return false;
    }

    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, data) {
        if (!cJSON_IsObject(it) || out.size() >= static_cast<size_t>(kMaxItems)) {
            continue;
        }
        cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON* title = cJSON_GetObjectItemCaseSensitive(it, "title");
        if (!cJSON_IsString(id) || id->valuestring == nullptr || !cJSON_IsString(title) ||
            title->valuestring == nullptr) {
            continue;
        }
        ChecklistItem item{};
        strlcpy(item.id, id->valuestring, sizeof(item.id));
        strlcpy(item.title, title->valuestring, sizeof(item.title));

        cJSON* plan_date = cJSON_GetObjectItemCaseSensitive(it, "planDate");
        if (cJSON_IsString(plan_date) && plan_date->valuestring != nullptr) {
            strlcpy(item.plan_date, plan_date->valuestring, sizeof(item.plan_date));
        }
        cJSON* plan_time = cJSON_GetObjectItemCaseSensitive(it, "planTime");
        if (cJSON_IsString(plan_time) && plan_time->valuestring != nullptr) {
            strlcpy(item.plan_time, plan_time->valuestring, sizeof(item.plan_time));
        }
        cJSON* status = cJSON_GetObjectItemCaseSensitive(it, "status");
        if (cJSON_IsString(status) && status->valuestring != nullptr) {
            strlcpy(item.status, status->valuestring, sizeof(item.status));
        } else {
            strlcpy(item.status, "pending", sizeof(item.status));
        }
        out.push_back(item);
    }
    return true;
}

static bool HttpJson(const char* method, const std::string& url, std::string& body_out, std::string& err_out) {
    body_out.clear();
    // 短时联网：等 WiFi → 请求；返回后 Release，WiFi 板可再 pause
    PowerNeedHold hold_net(PowerNeed::OtaDownload);

    if (!Board::GetInstance().EnsureNetworkReady()) {
        ESP_LOGW(TAG, "operation=http method=%s stage=network_ready result=unavailable", method);
        err_out = LangString_TASK_NET_NOT_READY;
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "operation=http method=%s stage=network_interface result=missing", method);
        err_out = LangString_TASK_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "operation=http method=%s stage=create_client result=failed", method);
        err_out = LangString_TASK_CONN_FAIL;
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    ApiApplyCommonHeaders(http);
    // 空 POST/PUT 必须 SetContent("")，否则 HttpClient 走 chunked 且不发结束块，服务端一直等 body
    if (method != nullptr && (std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0)) {
        http->SetContent(std::string());
    }
    if (!http->Open(method, url)) {
        ESP_LOGW(TAG, "operation=http method=%s stage=open error=%d", method, http->GetLastError());
        err_out = LangString_TASK_REQUEST_FAIL;
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "operation=http method=%s stage=response status=%d", method, status);
        err_out = body_out.empty() ? ("HTTP " + std::to_string(status)) : LangString_TASK_REQUEST_FAIL;
        return false;
    }
    return true;
}

static bool FetchChecklist(std::vector<ChecklistItem>& out, std::string& error) {
    std::string body;
    if (!HttpJson("GET", ApiChecklistItemsAllUrl(), body, error)) {
        return false;
    }
    if (!ParseItemsJson(body, out, error)) {
        ESP_LOGE(TAG, "operation=fetch stage=response result=invalid");
        return false;
    }
    return true;
}

static bool UpdateChecklist(const char* id, ChecklistOperation operation, std::string& error) {
    error.clear();
    if (!id || !*id) {
        error = LangString_TASK_INVALID;
        return false;
    }
    const bool complete = operation == ChecklistOperation::Complete;
    const char* fallback = complete ? LangString_TASK_COMPLETE_FAIL : LangString_TASK_DELETE_FAIL;
    std::string body;
    if (!HttpJson(complete ? "POST" : "DELETE", complete ? ApiChecklistItemCompleteUrl(id) : ApiChecklistItemUrl(id),
                  body, error)) {
        return false;
    }
    if (body.empty()) {
        return true;
    }
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> document(cJSON_Parse(body.c_str()), cJSON_Delete);
    const cJSON* root = document.get();
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const bool ok = cJSON_IsObject(root) && cJSON_IsNumber(code) && code->valueint == 0;
    if (!ok) {
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "msg");
        error = cJSON_IsString(message) ? message->valuestring : fallback;
        ESP_LOGE(TAG, "operation=update stage=response complete=%d result=invalid", complete);
    }
    return ok;
}

static void SortChecklistItems(std::vector<ChecklistItem>& items) {
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        const int date = std::strcmp(a.plan_date, b.plan_date);
        return date ? date > 0 : std::strcmp(a.plan_time, b.plan_time) > 0;
    });
}

static void RunChecklistRequest(const std::shared_ptr<ChecklistResult>& result, std::vector<std::string>& ids) {
    try {
        if (result->operation == ChecklistOperation::Refresh) {
            PowerNeedHold hold(PowerNeed::OtaDownload);
            std::vector<ChecklistItem> items;
            std::string error;
            if (FetchChecklist(items, error)) {
                SortChecklistItems(items);
                if (checklist_cache_replace(items.data(), items.size())) {
                    result->succeeded = 1;
                } else {
                    error = LangString_COMMON_MEMORY_RETRY;
                }
            }
            if (result->succeeded == 0) {
                result->failed = 1;
                strlcpy(result->error, error.c_str(), sizeof(result->error));
            }
        } else {
            // 批量请求持有整批网络占用，单项请求沿用HTTP层的作用域占用。
            std::optional<PowerNeedHold> batch_hold;
            if (result->batch) {
                batch_hold.emplace(PowerNeed::OtaDownload);
            }
            if (result->batch && !Board::GetInstance().EnsureNetworkReady()) {
                ESP_LOGW(TAG, "operation=batch stage=network_ready result=unavailable");
                result->failed = static_cast<int>(ids.size());
                strlcpy(result->error, LangString_TASK_NET_NOT_READY, sizeof(result->error));
            } else {
                for (size_t i = 0; i < ids.size(); ++i) {
                    std::string error;
                    bool ok = UpdateChecklist(ids[i].c_str(), result->operation, error);
                    if (!ok && result->batch) {
                        vTaskDelay(pdMS_TO_TICKS(400));
                        Board::GetInstance().EnsureNetworkReady();
                        error.clear();
                        ok = UpdateChecklist(ids[i].c_str(), result->operation, error);
                    }
                    if (ok) {
                        // 将成功ID移到已处理前缀，避免再分配一份批量结果数组。
                        if (static_cast<size_t>(result->succeeded) != i) {
                            ids[result->succeeded] = std::move(ids[i]);
                        }
                        ++result->succeeded;
                    } else {
                        ++result->failed;
                        if (result->error[0] == '\0') {
                            strlcpy(result->error, error.c_str(), sizeof(result->error));
                        }
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        result->failed = result->operation == ChecklistOperation::Refresh ? 1 :
                             static_cast<int>(ids.size()) - result->succeeded;
        strlcpy(result->error, LangString_COMMON_MEMORY_RETRY, sizeof(result->error));
        ESP_LOGE(TAG, "operation=run stage=allocate result=no_memory");
    }
    if (result->operation != ChecklistOperation::Refresh) {
        for (int i = 0; i < result->succeeded; ++i) {
            checklist_cache_apply_action(ids[i].c_str(), result->operation == ChecklistOperation::Delete);
        }
    }
    result->completed.store(true, std::memory_order_release);
    s_busy.store(false, std::memory_order_release);
}

ChecklistRequest ChecklistSubmit(ChecklistOperation operation, std::vector<std::string> ids, bool batch) {
    if (operation != ChecklistOperation::Refresh && operation != ChecklistOperation::Delete &&
        operation != ChecklistOperation::Complete) {
        return {};
    }
    if (operation == ChecklistOperation::Refresh) {
        if (!ids.empty() || batch) {
            return {};
        }
    } else if (ids.empty() || ids.size() > kMaxItems || (!batch && ids.size() != 1)) {
        return {};
    }
    for (const auto& id : ids) {
        if (id.empty() || id.size() >= CHECKLIST_CACHE_ID_LEN) {
            return {};
        }
    }
    if (s_busy.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    try {
        auto result = std::make_shared<ChecklistResult>();
        result->operation = operation;
        result->batch = batch;
        if (TaskExecutor::Schedule([result, ids = std::move(ids)]() mutable {
                RunChecklistRequest(result, ids);
            }, TaskDomain::NetworkIo)) {
            return result;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=submit stage=allocate result=no_memory");
    }
    s_busy.store(false, std::memory_order_release);
    return {};
}

bool ChecklistIsBusy() {
    return s_busy.load(std::memory_order_acquire);
}

bool ChecklistSyncCache() {
    if (!Board::GetInstance().IsNetworkReady() || s_busy.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    try {
        auto result = std::make_shared<ChecklistResult>();
        std::vector<std::string> ids;
        RunChecklistRequest(result, ids);
        return result->succeeded != 0;
    } catch (const std::bad_alloc&) {
        s_busy.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "operation=sync_cache stage=allocate result=no_memory");
        return false;
    }
}
