#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <arpa/inet.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "infra/storage/settings.h"
#include "infra/platform/system_info.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol(CloudConfiguration configuration) : configuration_(std::move(configuration)) {
    event_group_handle_ = xEventGroupCreate();
    if (event_group_handle_ == nullptr) {
        ESP_LOGE(TAG, "operation=initialize stage=event_group result=no_memory");
    }
}

WebsocketProtocol::~WebsocketProtocol() {
    websocket_.reset();
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return event_group_handle_ != nullptr;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected() || !packet) {
        return false;
    }
    if (packet->payload.empty() || packet->payload.size() > kMaxAudioPayloadBytes) {
        ESP_LOGE(TAG, "operation=send_audio result=invalid_size bytes=%u",
                 static_cast<unsigned>(packet->payload.size()));
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (event_group_handle_ == nullptr) {
        return false;
    }
    if (!configuration_.available) {
        ESP_LOGI(TAG, "websocket skipped: blank cloud endpoints");
        SetError(LangString_SERVER_NOT_CONNECTED);
        return false;
    }

    Settings settings(configuration_.websocket_namespace.c_str(), false);
    std::string url = !configuration_.websocket_url.empty() ? configuration_.websocket_url : settings.GetString("url");
    if (url.empty()) {
        ESP_LOGE(TAG, "Missing websocket URL in %s; OTA configuration is required",
                 configuration_.websocket_namespace.c_str());
        SetError(LangString_SERVER_NOT_CONNECTED);
        return false;
    }
    std::string token = configuration_.websocket_version ? configuration_.websocket_token : settings.GetString("token");
    int version = configuration_.websocket_version ? configuration_.websocket_version : settings.GetInt("version");
    if (version < 0 || version > 3) {
        ESP_LOGE(TAG, "operation=open_audio stage=protocol_version result=unsupported");
        return false;
    }
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    session_id_.clear();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "operation=open_audio stage=network result=unavailable");
        return false;
    }
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        try {
            if (binary) {
                if (on_incoming_audio_ != nullptr) {
                    size_t header_size = 0;
                    size_t payload_size = len;
                    uint32_t timestamp = 0;
                    bool valid = data != nullptr;
                    if (version_ == 2) {
                        BinaryProtocol2 header{};
                        header_size = sizeof(header);
                        valid = valid && len >= header_size;
                        if (valid) {
                            std::memcpy(&header, data, header_size);
                            payload_size = ntohl(header.payload_size);
                            timestamp = ntohl(header.timestamp);
                            valid = ntohs(header.version) == 2 && ntohs(header.type) == 0;
                        }
                    } else if (version_ == 3) {
                        BinaryProtocol3 header{};
                        header_size = sizeof(header);
                        valid = valid && len >= header_size;
                        if (valid) {
                            std::memcpy(&header, data, header_size);
                            payload_size = ntohs(header.payload_size);
                            valid = header.type == 0;
                        }
                    }
                    // 先验证实际帧长，再使用服务端声明的载荷长度。
                    if (!valid || len < header_size || payload_size == 0 || payload_size > kMaxAudioPayloadBytes ||
                        payload_size != len - header_size) {
                        ++rejected_audio_packets_;
                        if (rejected_audio_packets_ == 1 || rejected_audio_packets_ % 64 == 0) {
                            ESP_LOGW(TAG, "operation=receive_audio result=invalid_frame bytes=%u rejected=%u",
                                     static_cast<unsigned>(len), static_cast<unsigned>(rejected_audio_packets_));
                        }
                        return;
                    }
                    const auto* payload = reinterpret_cast<const uint8_t*>(data) + header_size;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(
                        AudioStreamPacket{.sample_rate = server_sample_rate_,
                                          .frame_duration = server_frame_duration_,
                                          .timestamp = timestamp,
                                          .payload = std::vector<uint8_t>(payload, payload + payload_size)}));
                }
            } else {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_owner(cJSON_ParseWithLength(data, len),
                                                                           cJSON_Delete);
                auto* root = root_owner.get();
                if (!cJSON_IsObject(root)) {
                    ++rejected_json_messages_;
                    if (rejected_json_messages_ == 1 || rejected_json_messages_ % 64 == 0) {
                        ESP_LOGW(TAG, "operation=receive_json result=invalid bytes=%u rejected=%u",
                                 static_cast<unsigned>(len), static_cast<unsigned>(rejected_json_messages_));
                    }
                    return;
                }
                auto type = cJSON_GetObjectItem(root, "type");
                if (cJSON_IsString(type)) {
                    if (strcmp(type->valuestring, "hello") == 0) {
                        ParseServerHello(root);
                    } else {
                        if (on_incoming_json_ != nullptr) {
                            on_incoming_json_(root);
                        }
                    }
                }
            }
            last_incoming_time_ = std::chrono::steady_clock::now();
        } catch (const std::bad_alloc&) {
            auto& rejected = binary ? rejected_audio_packets_ : rejected_json_messages_;
            ++rejected;
            if (rejected == 1 || rejected % 64 == 0) {
                ESP_LOGE(TAG, "operation=receive result=no_memory bytes=%u rejected=%u", static_cast<unsigned>(len),
                         static_cast<unsigned>(rejected));
            }
        }
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    const int64_t t0 = esp_timer_get_time();
    if (!websocket_->Connect(url.c_str())) {
        const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d elapsed_ms=%d free_int=%u",
                 websocket_->GetLastError(), static_cast<int>(elapsed_ms),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        SetError(LangString_SERVER_NOT_CONNECTED);
        return false;
    }
    ESP_LOGI(TAG, "Websocket TCP+handshake ok in %d ms", static_cast<int>((esp_timer_get_time() - t0) / 1000));

    std::string message;
    if (!BuildHelloMessage(version_, "websocket", message) || !SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(LangString_SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void WebsocketProtocol::CloseAudioChannel() {
    websocket_.reset();
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "unsupported websocket transport");
        return;
    }

    int sample_rate = server_sample_rate_;
    int frame_duration = server_frame_duration_;
    if (!ParseServerAudioParameters(root, sample_rate, frame_duration)) {
        return;
    }
    const auto* session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
    }
    server_sample_rate_ = sample_rate;
    server_frame_duration_ = frame_duration;

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        SetError(LangString_SERVER_ERROR);
        return false;
    }

    return true;
}
