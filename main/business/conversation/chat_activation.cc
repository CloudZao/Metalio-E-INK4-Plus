#include <new>
#include <cstdio>
#include <esp_log.h>
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "chat_activation.h"
#include "activation_polling.h"
#include "chat_cloud.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "business/yunxin_binding/yunxin_binding.h"
#include "infra/network/device_config_client.h"
#include "infra/platform/system_info.h"
#include "infra/storage/settings.h"
#include "sysmgmt/time/network_time.h"

static constexpr const char* TAG = "ChatActivation";

bool ChatActivation::IsReady() const {
    return phase_.load(std::memory_order_acquire) == Phase::Ready;
}
bool ChatActivation::IsChecking() const {
    return phase_.load(std::memory_order_acquire) == Phase::Checking;
}
bool ChatActivation::NeedsBinding() const {
    return binding_required_.load(std::memory_order_acquire);
}

static void ShowCode(const std::string& code) {
    const bool changed = ActivationPolling::GetInstance().SetCode(code);
    // 验证码只在屏幕显示，不写入日志。
    char status[64];
    std::snprintf(status, sizeof(status), LangString_ACTIVATION_CODE_FMT, code.c_str());

    if (changed) {
        ESP_LOGI(TAG, "activation required; code shown on display");
    }

    ConversationPresentation::SetStatus(status);
}

bool ChatActivation::Refresh() try {
    auto& board = Board::GetInstance();
    if (IsChecking() || !ChatCloudAvailable() || !board.IsNetworkReady() || board.IsWifiConfigMode() ||
        ActivationPolling::GetInstance().IsPaused()) {
        return false;
    }
    auto has_endpoint = []() {
        // Yunxin's transport is configured locally; activation uses its OTA endpoint.
        if (ChatCloudIsYunxin()) {
            return !ChatCloudYunxinWebsocketUrl(SystemInfo::GetMacAddress()).empty();
        }
        Settings ws(ChatCloudWebsocketNamespace());
        Settings mqtt(ChatCloudMqttNamespace());
        return !ws.GetString("url").empty() || !mqtt.GetString("endpoint").empty();
    };
    if (IsReady() && !binding_required_) {
        return false;
    }
    if (YunxinBinding::Enabled() && !YunxinBinding::IsDue()) {
        return false;
    }

    ActivationPollGuard poll_guard;
    if (!poll_guard) {
        return false;
    }

    phase_.store(Phase::Checking, std::memory_order_release);
    struct LoadingGuard {
        std::atomic<Phase>& phase;
        ~LoadingGuard() {
            if (phase.load() == Phase::Checking) {
                phase.store(Phase::Pending, std::memory_order_release);
            }
        }
    } guard{phase_};

    // Run at boot after the firmware check, then retry independently of chat entry.
    if (YunxinBinding::Enabled()) {
        bool active = false;
        const esp_err_t err = YunxinBinding::Check(active);
        if (err == ESP_OK && !active) {
            YunxinBindingNotifyUnlinked();
        }
        binding_required_ = !active;
        if (err != ESP_OK || !active) {
            return false;
        }
    } else {
        DeviceConfigClient client([] { return ActivationPolling::GetInstance().IsPaused(); },
                                  ChatCloudConfiguration(SystemInfo::GetMacAddress()));
        ESP_LOGI(TAG, "Chat activation check provider=%d pending=%d", static_cast<int>(ChatCloudActiveProvider()),
                 NeedsBinding());
        esp_err_t err = client.QueryChat();
        if (client.HasServerTime()) {
            TimeServiceCommitNetworkTime();
        }
        if (ActivationPolling::GetInstance().IsPaused()) {
            return false;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Chat activation check failed: %d", err);
            return false;
        }
        ESP_LOGI(TAG, "Chat configuration response: code=%d challenge=%d websocket=%d mqtt=%d",
                 client.HasActivationCode(), client.HasActivationChallenge(), client.HasWebsocketConfig(),
                 client.HasMqttConfig());
        if (client.HasActivationChallenge()) {
            binding_required_ = true;
            if (client.HasActivationCode() && client.GetActivationCode() != ActivationPolling::GetInstance().Code()) {
                ShowCode(client.GetActivationCode());
            }
            err = client.Activate();
            if (ActivationPolling::GetInstance().IsPaused()) {
                return false;
            }
            if (err == ESP_OK) {
                err = client.QueryChat();
                if (client.HasServerTime()) {
                    TimeServiceCommitNetworkTime();
                }
            }
        }
        if (client.HasActivationCode() || client.HasActivationChallenge()) {
            ESP_LOGI(TAG, "Chat activation still pending; retry remains enabled");
            binding_required_ = true;
            if (client.HasActivationCode()) {
                if (client.GetActivationCode() != ActivationPolling::GetInstance().Code()) {
                    ShowCode(client.GetActivationCode());
                }
            } else {
                ConversationPresentation::SetStatus(LangString_ACTIVATION);
            }
            return false;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Chat activation failed: %d", err);
            return false;
        }
        if (!has_endpoint()) {
            ESP_LOGE(TAG, "Chat configuration unavailable: err=%d websocket=%d mqtt=%d", err,
                     client.HasWebsocketConfig(), client.HasMqttConfig());
            return false;
        }
    }
    ActivationPolling::GetInstance().SetCode("");
    binding_required_ = false;
    phase_.store(Phase::Ready, std::memory_order_release);
    ESP_LOGI(TAG, "Chat activation confirmed; cached code cleared");
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=refresh result=no_memory");
    return false;
}
