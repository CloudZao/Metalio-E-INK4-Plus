#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "yunxin_binding.h"
#include "boards/common/board/board.h"
#include "business/conversation/chat_cloud.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "infra/platform/system_info.h"

static const char* TAG = "YunxinBinding";
static std::atomic<bool> linked{false};
static std::atomic<int64_t> next_poll_us{0};
static std::atomic<uint32_t> revision{0};
static std::mutex state_mutex;
static std::string qr_content;
static bool request_failed = false;

bool YunxinBinding::Enabled() {
    #if CONFIG_PRODUCT_GLOBAL
    return ChatCloudIsYunxin();
    #else
    return false;
    #endif
}

bool YunxinBinding::NeedsBinding() {
    return Enabled() && !linked.load(std::memory_order_acquire);
}

bool YunxinBinding::IsDue() {
    return esp_timer_get_time() >= next_poll_us;
}

static esp_err_t Query(bool& active, std::string& content, int& status, int& code, int& transport_error) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        return ESP_ERR_NO_MEM;
    }

    struct CloseGuard {
        Http* http;  // 请求结束时关闭连接

        ~CloseGuard() {
            http->Close();
        }
    } close{http.get()};

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    // 空正文必须显式设置，否则底层 POST 默认进入流式 chunked 上传模式。
    http->SetContent(std::string{});
    constexpr int kRequestBudgetMs = 15000;
    http->SetTimeout(kRequestBudgetMs);
    http->SetRequestDeadline(esp_timer_get_time() + int64_t(kRequestBudgetMs) * 1000);
    if (!http->Open("POST", yunxin_binding_url)) {
        transport_error = http->GetLastError();
        return ESP_FAIL;
    }
    status = http->GetStatusCode();
    if (status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::string body;
    char chunk[256];
    for (;;) {
        const int count = http->Read(chunk, sizeof(chunk));
        if (count < 0) {
            transport_error = http->GetLastError();
            return ESP_FAIL;
        }
        if (count == 0) {
            break;
        }
        if (body.size() + count > 4096) {
            return ESP_ERR_INVALID_SIZE;
        }
        body.append(chunk, count);
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (!json) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const auto* business_code = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
    if (!cJSON_IsNumber(business_code)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    code = business_code->valueint;
    if (business_code->valuedouble != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const auto* data = cJSON_GetObjectItemCaseSensitive(json.get(), "data");
    const auto* flag = cJSON_GetObjectItemCaseSensitive(data, "active");
    if (!cJSON_IsBool(flag)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    active = cJSON_IsTrue(flag);
    if (!active) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(data, "qrContent");
        if (!cJSON_IsString(value) || !value->valuestring[0]) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        content = value->valuestring;
        if (content.size() > 1024) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

esp_err_t YunxinBinding::Check(bool& active) {
    active = false;
    if (!Enabled() || !IsDue()) {
        return ESP_ERR_INVALID_STATE;
    }
    next_poll_us = esp_timer_get_time() + 3000000;
    PowerNeedHold hold(PowerNeed::OtaDownload);
    std::string content;
    int status = 0, code = -1, transport_error = 0;
    const esp_err_t result = Query(active, content, status, code, transport_error);
    static int64_t last_failure_us = 0;
    const int64_t now = esp_timer_get_time();
    if (result != ESP_OK) {
        active = false;
        if (last_failure_us == 0 || now - last_failure_us >= 30000000) {
            ESP_LOGW(TAG, "binding check retry err=%d http=%d code=%d transport=%d", result, status, code,
                     transport_error);
            last_failure_us = now;
        }
        if (status == 429 || code == 429) {
            next_poll_us = now + 10000000;
        }
    } else if (last_failure_us != 0) {
        ESP_LOGI(TAG, "binding check recovered");
        last_failure_us = 0;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        request_failed = result != ESP_OK;
        if (result == ESP_OK && !active) {
            qr_content = std::move(content);
        }
        if (active) {
            std::string{}.swap(qr_content);
        }
    }
    if (active) {
        linked.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "device binding confirmed");
    }
    revision.fetch_add(1, std::memory_order_release);
    return result;
}

YunxinBinding::Snapshot YunxinBinding::GetSnapshot() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return {qr_content, request_failed, revision.load(std::memory_order_acquire)};
}
