#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include "conversation_settings.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "business/conversation/chat_cloud.h"
#include "business/conversation/conversation_controller.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "ConversationSettings";
static constexpr int kHttpTimeoutMs = 15000;
static std::mutex s_mutex;
static std::weak_ptr<ConversationSettingsResult> s_request;
static std::weak_ptr<ConversationSettingsResult> s_provider_request;

static bool ParseTtsEnabled(const std::string& body, bool* enabled_out, std::string& err_out) {
    if (enabled_out == nullptr) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = LangString_SETTINGS_CONV_PARSE_FAIL;
        return false;
    }
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON* msg = cJSON_GetObjectItem(root, "msg");
        err_out = cJSON_IsString(msg) && msg->valuestring != nullptr ? msg->valuestring
                                                                     : LangString_SETTINGS_CONV_REQUEST_FAIL;
        cJSON_Delete(root);
        return false;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* en = data != nullptr ? cJSON_GetObjectItem(data, "enabled") : nullptr;
    if (!cJSON_IsBool(en)) {
        err_out = LangString_SETTINGS_CONV_MISSING_ENABLED;
        cJSON_Delete(root);
        return false;
    }
    *enabled_out = cJSON_IsTrue(en);
    cJSON_Delete(root);
    return true;
}

static bool HttpJson(const char* method, const std::string& url, const char* body_in, std::string& body_out,
                     std::string& err_out) {
    body_out.clear();
    if (url.empty()) {
        err_out = LangString_SETTINGS_CONV_REQUEST_FAIL;
        ESP_LOGI(TAG, "skip tts settings: cloud endpoints blank");
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = LangString_SETTINGS_CONV_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = LangString_SETTINGS_CONV_CONN_FAIL;
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    if (body_in != nullptr) {
        ApiApplyJsonHeaders(http);
        http->SetContent(std::string(body_in));
    } else {
        ApiApplyCommonHeaders(http);
    }
    std::string full = url;
    if (full.find('?') == std::string::npos) {
        full += "?Device-Id=";
        full += ApiDeviceId();
    }
    if (!http->Open(method, full)) {
        err_out = LangString_SETTINGS_CONV_REQUEST_FAIL;
        ESP_LOGW(TAG, "operation=tts stage=open result=failed");
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "operation=tts stage=response status=%d", status);
        err_out = body_out.empty() ? ("HTTP " + std::to_string(status)) : LangString_SETTINGS_CONV_REQUEST_FAIL;
        return false;
    }
    return true;
}

static void RunTtsRequest(ConversationSettingsResult& result, bool enabled) {
    if (ChatCloudUsesXiaozhiProtocol()) {
        result.ok = result.operation == ConversationSettingsOperation::QueryTts ||
                    ConversationController::GetInstance().SetLocalTtsEnabled(enabled);
        result.enabled = ChatCloudLocalTtsEnabled();
        if (!result.ok) {
            std::snprintf(result.error, sizeof(result.error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
        }
        return;
    }
    PowerNeedHold hold(PowerNeed::OtaDownload);
    std::string body;
    std::string error;
    if (!Board::GetInstance().EnsureNetworkReady()) {
        error = LangString_SETTINGS_CONV_NET_NOT_READY;
    } else if (result.operation == ConversationSettingsOperation::QueryTts) {
        result.ok = HttpJson("GET", ApiDeviceSettingsTtsUrl(), nullptr, body, error) &&
                    ParseTtsEnabled(body, &result.enabled, error);
    } else {
        char request[32];
        std::snprintf(request, sizeof(request), "{\"enabled\":%s}", enabled ? "true" : "false");
        if (HttpJson("PUT", ApiDeviceSettingsTtsUrl(), request, body, error)) {
            result.ok = ParseTtsEnabled(body, &result.enabled, error);
            if (!result.ok) {
                // 仅确认接纳的响应不能代表最终开关值，需重新读取。
                cJSON* root = cJSON_Parse(body.c_str());
                const cJSON* code = root ? cJSON_GetObjectItem(root, "code") : nullptr;
                const bool accepted = cJSON_IsNumber(code) && code->valueint == 0;
                cJSON_Delete(root);
                if (accepted) {
                    result.ok = HttpJson("GET", ApiDeviceSettingsTtsUrl(), nullptr, body, error) &&
                                ParseTtsEnabled(body, &result.enabled, error);
                }
            }
        }
    }
    if (!result.ok) {
        std::snprintf(result.error, sizeof(result.error), "%s",
                      error.empty() ? LangString_SETTINGS_CONV_REQUEST_FAIL : error.c_str());
    }
}

static ConversationSettingsRequest SubmitSettings(ConversationSettingsOperation operation, bool enabled,
                                                  ChatCloudProvider provider) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto& pending = operation == ConversationSettingsOperation::SaveProvider ? s_provider_request : s_request;
    const auto active = pending.lock();
    if (active && !active->completed.load(std::memory_order_acquire)) {
        return {};
    }
    auto result = std::make_shared<ConversationSettingsResult>();
    result->operation = operation;
    const TaskDomain domain = operation == ConversationSettingsOperation::SaveProvider ? TaskDomain::Storage
                              : ChatCloudUsesXiaozhiProtocol() ? TaskDomain::Voice : TaskDomain::NetworkIo;
    if (!TaskExecutor::Schedule([result, enabled, provider] {
        try {
            if (result->operation == ConversationSettingsOperation::SaveProvider) {
                result->ok = ChatCloudSaveProvider(provider);
                if (!result->ok) {
                    std::snprintf(result->error, sizeof(result->error), "%s", LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
                }
            } else {
                RunTtsRequest(*result, enabled);
            }
        } catch (const std::bad_alloc&) {
            result->ok = false;
            std::snprintf(result->error, sizeof(result->error), "%s", LangString_COMMON_MEMORY_RETRY);
            ESP_LOGE(TAG, "operation=settings stage=execute result=no_memory");
        }
        if (!result->ok) {
            ESP_LOGW(TAG, "operation=settings kind=%u result=failed", unsigned(result->operation));
        }
        result->completed.store(true, std::memory_order_release);
        if (result->operation == ConversationSettingsOperation::SaveProvider && result->ok) {
            SystemRequestReboot();
        }
    }, domain)) {
        return {};
    }
    pending = result;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=settings stage=submit result=no_memory");
    return {};
}

ConversationSettingsRequest ConversationSettingsQueryTts() {
    return SubmitSettings(ConversationSettingsOperation::QueryTts, false, kChatCloudDefaultProvider);
}

ConversationSettingsRequest ConversationSettingsSaveTts(bool enabled) {
    return SubmitSettings(ConversationSettingsOperation::SaveTts, enabled, kChatCloudDefaultProvider);
}

ConversationSettingsRequest ConversationSettingsSaveProvider(ChatCloudProvider provider) {
    return SubmitSettings(ConversationSettingsOperation::SaveProvider, false, provider);
}

ConversationSettingsRequest ConversationSettingsCurrent() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto provider = s_provider_request.lock();
    if (provider && !provider->completed.load(std::memory_order_acquire)) {
        return provider;
    }
    const auto result = s_request.lock();
    return result && !result->completed.load(std::memory_order_acquire) ? result : ConversationSettingsRequest{};
}
