#include <charconv>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <arpa/inet.h>
#include <esp_log.h>
#include "mqtt_protocol.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "infra/storage/settings.h"

#define TAG "MQTT"

static const char hex_chars[] = "0123456789ABCDEF";

static void RecordMqttRejectedJson(uint32_t& count, const char* reason) {
    ++count;
    if (count == 1 || count % 64 == 0) {
        ESP_LOGW(TAG, "operation=receive_json result=%s count=%lu", reason, static_cast<unsigned long>(count));
    }
}

// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return 0;  // 对于无效输入，返回0
}

MqttProtocol::MqttProtocol(std::function<bool(std::function<void()>)> schedule, std::function<bool()> can_reconnect,
                           CloudConfiguration configuration)
    : configuration_(std::move(configuration)),
      schedule_(std::move(schedule)),
      can_reconnect_(std::move(can_reconnect)) {
    event_group_handle_ = xEventGroupCreate();
    if (event_group_handle_ == nullptr) {
        ESP_LOGE(TAG, "operation=initialize stage=event_group result=no_memory");
        return;
    }

    esp_timer_create_args_t reconnect_timer_args = {
        .callback =
            [](void* arg) {
                auto* protocol = static_cast<MqttProtocol*>(arg);
                if (!protocol->alive_->load()) {
                    return;
                }
                try {
                    if (!protocol->can_reconnect_ || !protocol->can_reconnect_()) {
                        return;
                    }
                    ESP_LOGI(TAG, "Reconnecting to MQTT server");
                    if (protocol->schedule_ && protocol->schedule_([protocol, alive = protocol->alive_]() {
                            if (alive->load()) {
                                protocol->StartMqttClient(false);
                            }
                        })) {
                        return;
                    }
                    ESP_LOGW(TAG, "operation=reconnect stage=schedule result=unavailable");
                } catch (const std::bad_alloc&) {
                    ESP_LOGE(TAG, "operation=reconnect stage=schedule result=no_memory");
                }
                protocol->ScheduleReconnect();
            },
        .arg = this,
    };
    const auto result = esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "operation=initialize stage=reconnect_timer code=%d", static_cast<int>(result));
    }
}

MqttProtocol::~MqttProtocol() {
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        alive_->store(false);
    }
    ESP_LOGI(TAG, "MqttProtocol deinit");
    // 网络回调收束后不再有新的启动请求，再等待已经派发的定时器回调退出。
    mqtt_.reset();
    if (reconnect_timer_ != nullptr) {
        ESP_ERROR_CHECK(esp_timer_stop_blocking(reconnect_timer_, portMAX_DELAY));
        const auto result = esp_timer_delete(reconnect_timer_);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=deinitialize stage=reconnect_timer_delete code=%d", static_cast<int>(result));
        }
        reconnect_timer_ = nullptr;
    }
    udp_.reset();

    psa_destroy_key(aes_key_);

    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr || aes_key_ == PSA_KEY_ID_NULL || aes_nonce_.size() != 16 || !packet) {
        return false;
    }

    if (packet->payload.empty() || packet->payload.size() > kMaxAudioPayloadBytes) {
        ESP_LOGE(TAG, "operation=send_audio result=invalid_size bytes=%u",
                 static_cast<unsigned>(packet->payload.size()));
        return false;
    }
    std::string nonce(aes_nonce_);
    const uint16_t payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
    const uint32_t timestamp = htonl(packet->timestamp);
    const uint32_t sequence = htonl(++local_sequence_);
    memcpy(nonce.data() + 2, &payload_size, sizeof(payload_size));
    memcpy(nonce.data() + 8, &timestamp, sizeof(timestamp));
    memcpy(nonce.data() + 12, &sequence, sizeof(sequence));

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (!CryptCtr(reinterpret_cast<const uint8_t*>(nonce.data()), packet->payload.data(), packet->payload.size(),
                  reinterpret_cast<uint8_t*>(encrypted.data() + nonce.size()))) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    std::string message;
    if (!BuildHelloMessage(3, "udp", message) || !SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(LangString_SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "operation=create_mqtt result=no_network");
        return false;
    }
    udp_ = network->CreateUdp(2);
    if (!udp_) {
        return false;
    }
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        const auto reject = [this](const char* reason, size_t bytes) {
            ++rejected_audio_packets_;
            if (rejected_audio_packets_ == 1 || rejected_audio_packets_ % 64 == 0) {
                ESP_LOGW(TAG, "operation=receive_audio reason=%s bytes=%u rejected=%u", reason,
                         static_cast<unsigned>(bytes), static_cast<unsigned>(rejected_audio_packets_));
            }
        };
        if (aes_nonce_.size() != 16 || data.size() < 16) {
            reject("short_header", data.size());
            return;
        }
        if (data[0] != 0x01) {
            reject("invalid_type", data.size());
            return;
        }
        uint16_t declared_size;
        uint32_t timestamp, sequence;
        memcpy(&declared_size, data.data() + 2, sizeof(declared_size));
        memcpy(&timestamp, data.data() + 8, sizeof(timestamp));
        memcpy(&sequence, data.data() + 12, sizeof(sequence));
        timestamp = ntohl(timestamp);
        sequence = ntohl(sequence);
        const size_t payload_size = data.size() - 16;
        if (payload_size == 0 || payload_size > kMaxAudioPayloadBytes || ntohs(declared_size) != payload_size) {
            reject("invalid_length", data.size());
            return;
        }
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence,
                     remote_sequence_ + 1);
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        const auto* nonce = reinterpret_cast<const uint8_t*>(data.data());
        const auto* encrypted = nonce + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        if (!CryptCtr(nonce, encrypted, decrypted_size, packet->payload.data())) {
            ESP_LOGE(TAG, "Failed to decrypt audio data");
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    if (!udp_->Connect(udp_server_, udp_port_)) {
        udp_.reset();
        SetError(LangString_SERVER_NOT_CONNECTED);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (!alive_->load() || !event_group_handle_ || !reconnect_timer_) {
        return false;
    }
    if (!configuration_.available) {
        ESP_LOGI(TAG, "mqtt skipped: blank cloud endpoints");
        if (report_error) {
            SetError(LangString_SERVER_NOT_CONNECTED);
        }
        return false;
    }

    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings(configuration_.mqtt_namespace.c_str(), false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(LangString_SERVER_NOT_CONNECTED);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }
    mqtt_ = network->CreateMqtt(0);
    if (!mqtt_) {
        ESP_LOGE(TAG, "operation=create_mqtt result=no_memory");
        return false;
    }
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this, alive = alive_]() {
        if (!alive->load()) {
            return;
        }
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        ScheduleReconnect();
    });

    mqtt_->OnConnected([this, alive = alive_]() {
        if (!alive->load()) {
            return;
        }
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this, alive = alive_](const std::string& topic, const std::string& payload) {
        try {
            if (!alive->load()) {
                return;
            }
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_owner(cJSON_Parse(payload.c_str()), cJSON_Delete);
            auto* root = root_owner.get();
            if (!cJSON_IsObject(root)) {
                RecordMqttRejectedJson(rejected_json_packets_, "invalid_json");
                return;
            }
            cJSON* type = cJSON_GetObjectItem(root, "type");
            if (!cJSON_IsString(type)) {
                RecordMqttRejectedJson(rejected_json_packets_, "invalid_type");
                return;
            }

            if (strcmp(type->valuestring, "hello") == 0) {
                ParseServerHello(root);
            } else if (strcmp(type->valuestring, "goodbye") == 0) {
                auto session_id = cJSON_GetObjectItem(root, "session_id");
                if (session_id == nullptr || (cJSON_IsString(session_id) && session_id_ == session_id->valuestring)) {
                    schedule_([this, alive = alive_]() {
                        if (alive->load()) {
                            CloseAudioChannel();
                        }
                    });
                }
            } else if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }
            last_incoming_time_ = std::chrono::steady_clock::now();
        } catch (const std::bad_alloc&) {
            RecordMqttRejectedJson(rejected_json_packets_, "no_memory");
        }
    });

    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        const char* begin = endpoint.data() + pos + 1;
        const char* end = endpoint.data() + endpoint.size();
        const auto parsed = std::from_chars(begin, end, broker_port);
        if (broker_address.empty() || parsed.ec != std::errc() || parsed.ptr != end || broker_port <= 0 ||
            broker_port > 65535) {
            ESP_LOGE(TAG, "operation=connect_mqtt result=invalid_endpoint");
            return false;
        }
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        SetError(LangString_SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported UDP transport");
        return;
    }

    int sample_rate = server_sample_rate_;
    int frame_duration = server_frame_duration_;
    if (!ParseServerAudioParameters(root, sample_rate, frame_duration)) {
        return;
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    const auto* server = cJSON_GetObjectItem(udp, "server");
    const auto* port = cJSON_GetObjectItem(udp, "port");
    const auto* key_item = cJSON_GetObjectItem(udp, "key");
    const auto* nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || server->valuestring[0] == '\0' || !cJSON_IsNumber(port) ||
        port->valuedouble != port->valueint || port->valueint <= 0 || port->valueint > 65535 ||
        !cJSON_IsString(key_item) || std::strlen(key_item->valuestring) != 32 || !cJSON_IsString(nonce_item) ||
        std::strlen(nonce_item->valuestring) != 32) {
        ESP_LOGE(TAG, "Invalid UDP configuration");
        return;
    }
    udp_server_ = server->valuestring;
    udp_port_ = port->valueint;
    const auto* key = key_item->valuestring;
    const auto* nonce = nonce_item->valuestring;

    aes_nonce_ = DecodeHexString(nonce);
    const auto decoded_key = DecodeHexString(key);
    if (aes_nonce_.size() != 16 || decoded_key.size() != 16 || psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Invalid UDP AES configuration");
        return;
    }
    psa_destroy_key(aes_key_);
    aes_key_ = PSA_KEY_ID_NULL;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CTR);
    const auto status =
        psa_import_key(&attrs, reinterpret_cast<const uint8_t*>(decoded_key.data()), decoded_key.size(), &aes_key_);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "UDP AES key import failed: %ld", static_cast<long>(status));
        return;
    }
    local_sequence_ = 0;
    remote_sequence_ = 0;
    const auto* session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
    }
    server_sample_rate_ = sample_rate;
    server_frame_duration_ = frame_duration;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    if (hex_string.size() % 2 != 0 || hex_string.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return {};
    }
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

// CTR applies the same keystream in both directions. PSA replaces the removed
// mbedtls/aes.h API in ESP-IDF 6; each packet owns a fresh counter operation.
bool MqttProtocol::CryptCtr(const uint8_t* nonce, const uint8_t* input, size_t length, uint8_t* output) {
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t written = 0, final_size = 0;
    auto status = psa_cipher_encrypt_setup(&op, aes_key_, PSA_ALG_CTR);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&op, nonce, 16);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&op, input, length, output, length, &written);
    }
    uint8_t final_block[16];
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&op, final_block, sizeof(final_block), &final_size);
    }
    psa_cipher_abort(&op);
    return status == PSA_SUCCESS && written == length && final_size == 0;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (!mqtt_ || publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        SetError(LangString_SERVER_ERROR);
        return false;
    }
    return true;
}

void MqttProtocol::ScheduleReconnect() {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (!alive_->load() || reconnect_timer_ == nullptr) {
        return;
    }
    const auto result = esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "operation=reconnect stage=timer_start code=%d", static_cast<int>(result));
    }
}
