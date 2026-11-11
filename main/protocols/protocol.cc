#include "protocol.h"

#include <esp_log.h>
#include <memory>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
                      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}

bool Protocol::SendImageQuestion(const std::string& image_id, bool recorded) {
    const auto encoding_failed = []() {
        ESP_LOGE(TAG, "image question failed stage=encode");
        return false;
    };
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "session_id", session_id_.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "type", "listen") ||
        !cJSON_AddStringToObject(root.get(), "state", recorded ? "stop" : "detect")) return encoding_failed();
    if (!recorded && !cJSON_AddStringToObject(root.get(), "mode", "auto")) return encoding_failed();
    auto* images = cJSON_AddArrayToObject(root.get(), "image_ids");
    auto* image = cJSON_CreateString(image_id.c_str());
    if (!images || !image) {
        cJSON_Delete(image);
        return encoding_failed();
    }
    cJSON_AddItemToArray(images, image);
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!json) return encoding_failed();
    if (!SendText(json.get())) {
        ESP_LOGW(TAG, "image question failed stage=send; photo retained");
        return false;
    }
    ESP_LOGI(TAG, "image question submitted images=1 recorded=%d", recorded ? 1 : 0);
    return true;
}
