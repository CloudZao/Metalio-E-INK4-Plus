#include <cstring>
#include <new>
#include <esp_app_desc.h>
#include <esp_log.h>
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "conversation_controller.h"
#include "assets/lang_config.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "business/conversation/activation_polling.h"
#include "business/conversation/chat_cloud.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "infra/diagnostics/firmware_memory.h"
#include "infra/platform/system_info.h"
#include "infra/protocols/mcp/mcp_server.h"
#include "infra/protocols/mqtt_protocol.h"
#include "infra/protocols/websocket_protocol.h"
#include "infra/storage/settings.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_notice.h"

static constexpr const char* TAG = "ConversationProtocol";

static bool ValidConversationEnvelope(const cJSON* root) {
    const auto* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsObject(root) || !cJSON_IsString(type) || type->valuestring == nullptr) {
        ESP_LOGW(TAG, "operation=receive_json stage=type result=invalid");
        return false;
    }
    if (strcmp(type->valuestring, "tts") == 0) {
        const auto* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (!cJSON_IsString(state) || state->valuestring == nullptr) {
            ESP_LOGW(TAG, "operation=receive_json stage=tts_state result=invalid");
            return false;
        }
    }
    return true;
}

void ConversationController::EnsureProtocolReady() {
    if (!ChatCloudAvailable()) {
        ESP_LOGI(TAG, "EnsureProtocolReady skipped: blank cloud endpoints");
        protocol_.reset();
        return;
    }
    if (protocol_) {
        return;
    }
    // Activation is checked during startup/background retries, never on chat entry.
    if (!activation_.IsReady() || activation_.NeedsBinding()) {
        ESP_LOGI(TAG, "Chat protocol deferred: ready=%d activation_pending=%d", activation_.IsReady(),
                 activation_.NeedsBinding());
        ActivationPolling::GetInstance().RequestCheck();
        if (ActivationPolling::GetInstance().HasCode()) {
            UiRestoreIdleStatus();
        }
        return;
    }

    ConversationPresentation::SetStatus(LangString_LOADING_PROTOCOL);

    Settings ws(ChatCloudWebsocketNamespace());
    Settings mqtt(ChatCloudMqttNamespace());
    if (!ChatCloudIsYunxin() && ws.GetString("url").empty() && !mqtt.GetString("endpoint").empty()) {
        protocol_ = std::make_unique<MqttProtocol>(
            [this](std::function<void()> task) { return TaskExecutor::Schedule(std::move(task)); },
            [this]() { return SystemState::GetInstance().Device() == kDeviceStateIdle; },
            ChatCloudConfiguration(SystemInfo::GetMacAddress()));
    } else {
        protocol_ = std::make_unique<WebsocketProtocol>(ChatCloudConfiguration(SystemInfo::GetMacAddress()));
    }

    const auto generation = requests_.Generation();
    const auto current_session = [this, generation]() {
        return generation == requests_.Generation() && ConversationPresentation::IsActive() && !PowerLifecycleIsStandby();
    };
    protocol_->OnConnected([this, current_session]() {
        if (current_session()) {
            UiDismissSystemAlert();
        }
    });

    protocol_->OnNetworkError([this, generation, current_session](const std::string& message) {
        if (!current_session()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_message_ = message;
            last_error_generation_ = generation;
        }
        PostEvents(MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this, current_session](std::unique_ptr<AudioStreamPacket> packet) {
        if (current_session() && SystemState::GetInstance().Device() == kDeviceStateSpeaking &&
            ChatCloudSpeechPlaybackEnabled()) {
            AudioRuntime::GetInstance().Service().PushPacketToDecodeQueue(std::move(packet));
        }
    });
    auto* session = protocol_.get();
    protocol_->OnAudioChannelOpened([session]() {
        FirmwareMemoryCheckpoint("chat-channel-open");
        auto& board = Board::GetInstance();
        auto codec = board.GetAudioCodec();
        PowerPolicy::GetInstance().Acquire(PowerNeed::AudioSession);
        if (session->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(
                TAG,
                "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                session->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, current_session]() {
        PowerPolicy::GetInstance().Release(PowerNeed::AudioSession);
        auto after = [this, current_session]() {
            if (!current_session()) {
                return;
            }

            ConversationPresentation::SetDisplayMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        };
        if (ChatCloudIsYunxin()) {
            QueueYunxinTextBoundary(std::move(after));
        } else {
            TaskExecutor::Schedule(std::move(after));
        }
    });
    protocol_->OnIncomingJson([this, current_session](const cJSON* root) {
        if (!current_session() || !ValidConversationEnvelope(root)) {
            return;
        }
        try {

            auto type = cJSON_GetObjectItem(root, "type");
            if (strcmp(type->valuestring, "tts") == 0) {
                auto state = cJSON_GetObjectItem(root, "state");
                if (strcmp(state->valuestring, "start") == 0) {
                    TaskExecutor::Schedule([this, current_session]() {
                        if (!current_session()) {
                            return;
                        }
                        if (ChatCloudActiveProvider() == ChatCloudProvider::Baiwen && ConversationPresentation::IsPttHeld()) {
                            return;
                        }
                        if (ChatCloudSpeechPlaybackEnabled() &&
                            (SystemState::GetInstance().Device() == kDeviceStateIdle ||
                             SystemState::GetInstance().Device() == kDeviceStateListening)) {
                            SetDeviceState(kDeviceStateSpeaking);
                        }
                    });
                } else if (strcmp(state->valuestring, "stop") == 0) {
                    auto after = [this, current_session]() {
                        if (!current_session()) {
                            return;
                        }
                        if (!question_.image_id.empty()) {
                            question_.image_id.clear();
                            ConversationPresentation::FinishPhotoQuestion();
                        }
                        if (SystemState::GetInstance().Device() == kDeviceStateSpeaking) {
                            if (listening_mode_ == kListeningModeManualStop) {
                                SetDeviceState(kDeviceStateIdle);
                            } else {
                                SetDeviceState(kDeviceStateListening);
                            }
                        }
                    };
                    if (ChatCloudIsYunxin()) {
                        QueueYunxinTextBoundary(std::move(after));
                    } else {
                        TaskExecutor::Schedule(std::move(after));
                    }
                } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                    auto text = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(text)) {
                        if (ChatCloudIsYunxin()) {
                            ReceiveYunxinText(text->valuestring);
                        } else {
                            TaskExecutor::Schedule(
                                [current_session, message = std::string(text->valuestring)]() {
                                    if (!current_session()) {
                                        return;
                                    }
                                    ConversationPresentation::SetDisplayMessage("assistant", message.c_str());
                                });
                        }
                    }
                }
            } else if (strcmp(type->valuestring, "stt") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    auto after = [current_session, message = std::string(text->valuestring)]() {
                        if (!current_session()) {
                            return;
                        }
                        ConversationPresentation::SetDisplayMessage("user", message.c_str());
                    };
                    if (ChatCloudIsYunxin()) {
                        QueueYunxinTextBoundary(std::move(after));
                    } else {
                        TaskExecutor::Schedule(std::move(after));
                    }
                }
            } else if (strcmp(type->valuestring, "vision") == 0) {
                const auto* state = cJSON_GetObjectItem(root, "state");
                const auto* code = cJSON_GetObjectItem(root, "code");
                if (cJSON_IsString(state) &&
                    (strcmp(state->valuestring, "error") == 0 || strcmp(state->valuestring, "missing_images") == 0)) {
                    const bool reupload =
                        strcmp(state->valuestring, "missing_images") == 0 ||
                        (cJSON_IsString(code) && (strcmp(code->valuestring, "VISION_CONTEXT_EXPIRED") == 0 ||
                                                  strcmp(code->valuestring, "VISION_CONTEXT_UNAVAILABLE") == 0));
                    const bool disabled = cJSON_IsString(code) && strcmp(code->valuestring, "VISION_DISABLED") == 0;
                    const char* safe_code = "unknown";
                    constexpr const char* codes[] = {"INVALID_IMAGE_REFS",     "INVALID_TEXT",
                                                     "VISION_DISABLED",        "VISION_IMAGE_LIMIT",
                                                     "VISION_MODEL_FAILED",    "VISION_CONTEXT_EXPIRED",
                                                     "VISION_CONTEXT_CHANGED", "VISION_CONTEXT_UNAVAILABLE",
                                                     "VISION_FAILED"};
                    if (cJSON_IsString(code)) {
                        for (const auto* known : codes) {
                            if (strcmp(code->valuestring, known) == 0) {
                                safe_code = known;
                                break;
                            }
                        }
                    } else if (strcmp(state->valuestring, "missing_images") == 0) {
                        safe_code = "missing_images";
                    }
                    TaskExecutor::Schedule([this, current_session, reupload, disabled, safe_code]() {
                        if (!current_session() || question_.image_id.empty()) {
                            return;
                        }
                        ESP_LOGW(TAG, "image question rejected stage=vision_response code=%s; photo retained",
                                 safe_code);
                        ConversationPresentation::PhotoQuestionError(question_.image_id, reupload, disabled);
                    });
                }
            } else if (strcmp(type->valuestring, "llm") == 0) {
                auto emotion = cJSON_GetObjectItem(root, "emotion");
                if (cJSON_IsString(emotion)) {
                    TaskExecutor::Schedule(
                        [this, current_session, emotion_str = std::string(emotion->valuestring)]() {
                            if (!current_session()) {
                                return;
                            }
                            ConversationPresentation::SetDisplayEmotion(emotion_str.c_str());
                        });
                }
            } else if (strcmp(type->valuestring, "mcp") == 0) {
                auto payload = cJSON_GetObjectItem(root, "payload");
                if (cJSON_IsObject(payload)) {
                    McpServer::GetInstance().ParseMessage(payload);
                }
            } else if (strcmp(type->valuestring, "system") == 0) {
                auto command = cJSON_GetObjectItem(root, "command");
                if (cJSON_IsString(command)) {
                    if (strcmp(command->valuestring, "reboot") == 0) {
                        TaskExecutor::Schedule([this, current_session]() {
                            if (current_session()) {
                                SystemRequestReboot();
                            }
                        });
                    } else {
                        ESP_LOGW(TAG, "Unknown system command");
                    }
                }
            } else if (strcmp(type->valuestring, "alert") == 0) {
                auto status = cJSON_GetObjectItem(root, "status");
                auto message = cJSON_GetObjectItem(root, "message");
                auto emotion = cJSON_GetObjectItem(root, "emotion");
                if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                    UiShowSystemAlert(status->valuestring, message->valuestring, emotion->valuestring);
                } else {
                    ESP_LOGW(TAG, "UiShowSystemAlert command requires status, message and emotion");
                }
            #if CONFIG_RECEIVE_CUSTOM_MESSAGE
            } else if (strcmp(type->valuestring, "custom") == 0) {
                auto payload = cJSON_GetObjectItem(root, "payload");
                if (cJSON_IsObject(payload)) {
                    std::unique_ptr<char, decltype(&cJSON_free)> printed(cJSON_PrintUnformatted(payload), cJSON_free);
                    if (!printed) {
                        ESP_LOGE(TAG, "operation=receive_json stage=custom_format result=no_memory");
                        return;
                    }
                    TaskExecutor::Schedule(
                        [this, current_session, payload_str = std::string(printed.get())]() {
                            if (!current_session())
                                return;
                            ConversationPresentation::SetDisplayMessage("system", payload_str.c_str());
                        });
                } else {
                    ESP_LOGW(TAG, "Invalid custom message format: missing payload");
                }
            #endif
            } else {
                ESP_LOGW(TAG, "Unknown message type");
            }
        } catch (const std::bad_alloc&) {
            ESP_LOGE(TAG, "operation=receive_json result=no_memory");
        }
    });

    FirmwareMemoryCheckpoint("chat-protocol-before-start");
    bool protocol_started = protocol_->Start();
    FirmwareMemoryCheckpoint("chat-protocol-started");
    if (!protocol_started) {
        TeardownProtocol();
        return;
    }
    if (ChatCloudIsYunxin() && yunxin_text_timer_handle_ == nullptr) {
        esp_timer_create_args_t yunxin_timer_args = {
            .callback = [](void* arg) { static_cast<ConversationController*>(arg)->QueueYunxinTextUpdate(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "yx_text",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&yunxin_timer_args, &yunxin_text_timer_handle_) != ESP_OK) {
            ESP_LOGW(TAG, "NetEase text timer unavailable; render at sentence boundaries");
        }
    }
    if (ChatCloudIsYunxin() && yunxin_text_timer_handle_ != nullptr &&
        !esp_timer_is_active(yunxin_text_timer_handle_)) {
        esp_timer_start_periodic(yunxin_text_timer_handle_, 500000);
    }

    const std::string version = esp_app_get_description()->version;
    if (protocol_started && !version.empty()) {

        std::string message = std::string(LangString_VERSION) + version;
        ConversationPresentation::ShowNotification(message.c_str());
        ConversationPresentation::SetDisplayMessage("system", "");
    }
}

void ConversationController::TeardownProtocol() {
    if (yunxin_text_timer_handle_ != nullptr) {
        esp_timer_stop(yunxin_text_timer_handle_);
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    if (ChatCloudIsYunxin()) {
        for (const auto& snapshot : yunxin_text_inbox_.Cancel()) {
            ApplyYunxinText(snapshot);
        }
    }
}

void ConversationController::SendMcpMessage(const std::string& payload) {
    // protocol_ 可能在排队期间被 TeardownProtocol() 拆掉（离开助手页）。
    // 真正发送时必须再判空，避免 LoadProhibited。
    if (TaskExecutor::IsCurrent(TaskDomain::Voice)) {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        return;
    }

    TaskExecutor::Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}
