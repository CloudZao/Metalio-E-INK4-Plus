#include <cstring>
#include <memory>
#include <new>
#include <esp_log.h>
#include "protocol.h"

#define TAG "Protocol"

bool Protocol::BuildHelloMessage(int version, const char* transport, std::string& message) const try {
    message.clear();
    const auto failed = []() {
        ESP_LOGE(TAG, "operation=hello stage=encode result=no_memory");
        return false;
    };
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "type", "hello") ||
        !cJSON_AddNumberToObject(root.get(), "version", version) ||
        !cJSON_AddStringToObject(root.get(), "transport", transport)) {
        return failed();
    }
    auto* features = cJSON_AddObjectToObject(root.get(), "features");
    if (!features || !cJSON_AddBoolToObject(features, "mcp", true)) {
        return failed();
    }
    auto* audio_params = cJSON_AddObjectToObject(root.get(), "audio_params");
    if (!audio_params || !cJSON_AddStringToObject(audio_params, "format", "opus") ||
        !cJSON_AddNumberToObject(audio_params, "sample_rate", 16000) ||
        !cJSON_AddNumberToObject(audio_params, "channels", 1) ||
        !cJSON_AddNumberToObject(audio_params, "frame_duration", kOpusFrameDurationMs)) {
        return failed();
    }
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!json) {
        return failed();
    }
    message = json.get();
    return true;
} catch (const std::bad_alloc&) {
    message.clear();
    ESP_LOGE(TAG, "operation=hello stage=copy result=no_memory");
    return false;
}

bool Protocol::ParseServerAudioParameters(const cJSON* root, int& sample_rate, int& frame_duration) {
    const auto* params = cJSON_GetObjectItem(root, "audio_params");
    if (params == nullptr) {
        return true;
    }
    const auto* rate = cJSON_GetObjectItem(params, "sample_rate");
    const auto* duration = cJSON_GetObjectItem(params, "frame_duration");
    const auto* channels = cJSON_GetObjectItem(params, "channels");
    const auto* format = cJSON_GetObjectItem(params, "format");
    if (!cJSON_IsObject(params) ||
        (rate != nullptr && (!cJSON_IsNumber(rate) || rate->valuedouble != rate->valueint)) ||
        (duration != nullptr && (!cJSON_IsNumber(duration) || duration->valuedouble != duration->valueint)) ||
        (channels != nullptr && (!cJSON_IsNumber(channels) || channels->valuedouble != 1)) ||
        (format != nullptr && (!cJSON_IsString(format) || std::strcmp(format->valuestring, "opus") != 0))) {
        ESP_LOGE(TAG, "operation=hello stage=audio_params result=invalid_fields");
        return false;
    }
    const int next_rate = rate != nullptr ? rate->valueint : sample_rate;
    const int next_duration = duration != nullptr ? duration->valueint : frame_duration;
    if (!IsValidOpusStreamParameters(next_rate, next_duration)) {
        ESP_LOGE(TAG, "operation=hello stage=audio_params result=unsupported rate=%d duration_ms=%d", next_rate,
                 next_duration);
        return false;
    }
    sample_rate = next_rate;
    frame_duration = next_duration;
    return true;
}

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

void Protocol::SendAbortSpeaking() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";

    message += "}";
    SendText(message);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeAutoStop) {
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
        !cJSON_AddStringToObject(root.get(), "state", recorded ? "stop" : "detect"))
        return encoding_failed();
    if (!recorded && !cJSON_AddStringToObject(root.get(), "mode", "auto")) {
        return encoding_failed();
    }
    auto* images = cJSON_AddArrayToObject(root.get(), "image_ids");
    auto* image = cJSON_CreateString(image_id.c_str());
    if (!images || !image) {
        cJSON_Delete(image);
        return encoding_failed();
    }
    cJSON_AddItemToArray(images, image);
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!json) {
        return encoding_failed();
    }
    if (!SendText(json.get())) {
        ESP_LOGW(TAG, "image question failed stage=send; photo retained");
        return false;
    }
    ESP_LOGI(TAG, "image question submitted images=1 recorded=%d", recorded ? 1 : 0);
    return true;
}
