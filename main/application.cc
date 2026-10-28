#include "ssid_manager.h"
#include "application.h"
#include "board.h"
#include "power_policy.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "metalio_audio.h"
#include "websocket_protocol.h"
#include "mqtt_protocol.h"
#include "assets/lang_config.h"
#include "assets/lang_bluetooth.h"
#include "mcp_server.h"
#include "assets.h"
#include "assistant_screen/assistant_screen.h"
#include "settings_screen/settings_test/settings_test_screens.h"
#include "a2ui_img_cache.h"
#include "assistant_screen/assistant_chat_store.h"
#include "boot_key_handler.h"
#include "screen_common.h"
#include "fontpack_lvgl.h"
#include "settings.h"
#include "standby_screen/standby_screen.h"
#include "task_screen/task_screen.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
#include "boards/metalio-e-ink4-plus/display/metalio_touch.h"
#include "boards/metalio-e-ink4-plus/bus/metalio_i2c1.h"
#endif
#include "ota_ignore.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_sync.h"
#include "reader/book_library_warmup.h"
#include "device_wifi_location.h"
#include "api_endpoints.h"
#include "chat_cloud.h"
#include "firmware_memory.h"
#include "SdCardManager.hpp"
#include "sd_log_writer.h"

#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <array>
#include <algorithm>
#include <thread>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#define TAG "Application"

namespace {

// Created only after confirmation. Nested refresh pauses cannot resume the
// sampler while this scope is alive; all failure returns restore our one pause.
class OtaTouchPause {
public:
    OtaTouchPause() {
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
        metalio_touch_set_feed_paused(true);
        // Drain an in-flight read before starting the upgrade. The sampler also
        // rechecks pause depth after taking this bus lock, closing the race.
        if (metalio_i2c1_lock(-1)) {
            metalio_i2c1_unlock();
        }
#endif
    }
    ~OtaTouchPause() {
#ifdef CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS
        metalio_touch_set_feed_paused(false);
#endif
    }
    OtaTouchPause(const OtaTouchPause&) = delete;
    OtaTouchPause& operator=(const OtaTouchPause&) = delete;
};

constexpr int kAssistantNetSliceMs = 200;
constexpr int kAssistantNetBudgetMs = 30000;

/** 百问语音任务分片等待网络，离页或待机时结束。 */
bool EnsureAssistantNetworkReady(const std::function<bool()>& still_needed = {}) {
    auto& board = Board::GetInstance();
    if (still_needed && !still_needed()) return false;
    if (!chat_cloud::Available()) {
        ESP_LOGI(TAG, "assistant network skipped: blank cloud endpoints");
        return false;
    }
    if (board.IsNetworkReady()) {
        return !still_needed || still_needed();
    }
    if (board.IsWifiConfigMode()) {
        return false;
    }
    if (board.GetNetworkErrorText(nullptr)) {
        board.EnsureNetworkReady(0);
        return false;
    }
    for (int elapsed = 0; elapsed < kAssistantNetBudgetMs; elapsed += kAssistantNetSliceMs) {
        if (!AssistantScreen::IsActive() || StandbyScreen::IsActive() || board.GetNetworkErrorText(nullptr) ||
            (still_needed && !still_needed())) {
            ESP_LOGI(TAG, "assistant network wait aborted: left screen or standby");
            return false;
        }
        if (board.EnsureNetworkReady(kAssistantNetSliceMs)) {
            // 连上后立刻刷成 RSSI 档，勿等被挡住的 CLOCK_TICK
            if (auto* display = board.GetDisplay()) {
                display->UpdateStatusBar(true);
            }
            return !still_needed || still_needed();
        }
        // 每秒更新一次当前业务的联网状态。
        if ((elapsed % 1000) == 0) {
            if (auto* display = board.GetDisplay()) {
                display->UpdateStatusBar();
            }
        }
    }
    ESP_LOGW(TAG, "assistant network wait timeout");
    // 页面保网和普通空闲断网由 PowerPolicy 统一决定，不在业务超时后暂停 STA。
    return false;
}

}  // namespace

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

}

Application::~Application() {
    if (yunxin_text_timer_handle_ != nullptr) {
        esp_timer_stop(yunxin_text_timer_handle_);
        esp_timer_delete(yunxin_text_timer_handle_);
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::AcquireAudioService(AudioClient client, std::function<bool()> still_needed) {
    std::lock_guard<std::recursive_mutex> lock(audio_service_mutex_);
    if (still_needed && !still_needed()) return false;
    const auto bit = static_cast<uint32_t>(client);
    audio_release_pending_.fetch_and(~bit, std::memory_order_acq_rel);
    audio_clients_ |= bit;
    EnsureAudioServiceRunning();
    if (!audio_running_ || (still_needed && !still_needed())) {
        ReleaseAudioClients(bit);
        return false;
    }
    return true;
}

void Application::ReleaseAudioService(AudioClient client) {
    std::lock_guard<std::recursive_mutex> lock(audio_service_mutex_);
    ReleaseAudioClients(static_cast<uint32_t>(client));
}

void Application::ReleaseAudioClients(uint32_t clients) {
    // 调用方持有音频服务锁；保留联网应用标记，直到最后一个音频工作线程退出。
    constexpr uint32_t network_clients = static_cast<uint32_t>(AudioClient::Assistant) |
                                         static_cast<uint32_t>(AudioClient::WorkBuddy);
    const auto previous = audio_clients_.load();
    const auto remaining = previous & ~clients;
    audio_clients_.store(remaining | (previous & clients & network_clients));
    if (remaining == 0) StopAudioServiceIfRunning();
    audio_clients_.store(remaining, std::memory_order_release);
}

void Application::RequestAudioServiceRelease(AudioClient client) {
    audio_release_pending_.fetch_or(static_cast<uint32_t>(client), std::memory_order_release);
    xEventGroupSetBits(event_group_, MAIN_EVENT_RELEASE_AUDIO);
}

void Application::EnsureAudioServiceRunning() {
    std::lock_guard<std::recursive_mutex> audio_lock(audio_service_mutex_);
    // 与 397 一致：先等 BH 模式1（提供 I2S 时钟），再 Start codec
    if (!MetalioAudio_ModeReady()) {
        MetalioAudio_RequestModeInit();
        return;
    }

    if (!audio_initialized_) {
        auto codec = Board::GetInstance().GetAudioCodec();
        audio_service_.Initialize(codec);

        AudioServiceCallbacks callbacks;
        callbacks.on_send_queue_available = [this]() {
            xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
        };
        callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
            (void)wake_word;
            xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
        };
        callbacks.on_vad_change = [this](bool speaking) {
            (void)speaking;
            xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
        };
        audio_service_.SetCallbacks(callbacks);
        audio_initialized_ = true;
    }

    if (!audio_running_) {
        audio_service_.Start();
        if (!audio_service_.IsStarted()) {
            ESP_LOGE(TAG, "Audio service Start failed");
            return;
        }
        audio_running_ = true;
        ESP_LOGI(TAG, "Audio service started");
    }
}

void Application::StopAudioServiceIfRunning() {
    std::lock_guard<std::recursive_mutex> audio_lock(audio_service_mutex_);
    if (SettingsTestAgingScreen::IsActive() ||
        (audio_clients_ & static_cast<uint32_t>(AudioClient::AgingTest))) {
        ESP_LOGW(TAG, "Audio stop skipped: aging test owns audio");
        return;
    }
    if (!audio_running_) {
        return;
    }
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.Stop();
    audio_running_ = false;
    ESP_LOGI(TAG, "Audio service stopped");
}

void Application::EnsureVoiceModelsReady() {
    if (voice_models_ready_) {
        return;
    }
    // Assets::Apply 可能已注入 srmodels；此处与原先开机路径一致，再用 model 分区覆盖/补齐
    audio_service_.SetModelsList(esp_srmodel_init("model"));
    voice_models_ready_ = true;
}

void Application::CheckChatActivation() {
    auto& board = Board::GetInstance();
    if (chat_config_loading_ || !chat_cloud::Available() || !board.IsNetworkReady() ||
        board.IsWifiConfigMode() || activation_paused_.load(std::memory_order_acquire)) {
        return;
    }
    auto has_endpoint = []() {
        // Yunxin's transport is configured locally; activation uses its OTA endpoint.
        if (chat_cloud::IsYunxin()) {
            return !chat_cloud::YunxinWebsocketUrl(SystemInfo::GetMacAddress()).empty();
        }
        Settings ws(chat_cloud::WebsocketNamespace());
        Settings mqtt(chat_cloud::MqttNamespace());
        return !ws.GetString("url").empty() || !mqtt.GetString("endpoint").empty();
    };
    if (chat_config_ready_ && !chat_activation_pending_) { return; }

    {
        std::lock_guard<std::mutex> lock(activation_poll_mutex_);
        if (activation_pause_count_ || activation_poll_busy_) return;
        activation_poll_busy_ = true;
    }
    struct PollGuard {
        std::mutex& mutex;
        bool& busy;
        ~PollGuard() { std::lock_guard<std::mutex> lock(mutex); busy = false; }
    } poll_guard{activation_poll_mutex_, activation_poll_busy_};
    chat_config_loading_ = true;
    struct LoadingGuard {
        std::atomic<bool>& loading;
        ~LoadingGuard() { loading.store(false, std::memory_order_release); }
    } guard{chat_config_loading_};
    // Run at boot after the firmware check, then retry independently of chat entry.
    Ota ota;
    ESP_LOGI(TAG, "Chat activation check provider=%d pending=%d",
             static_cast<int>(chat_cloud::ActiveProvider()), chat_activation_pending_.load());
    esp_err_t err = ota.CheckChatConfiguration();
    if (activation_paused_.load(std::memory_order_acquire)) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Chat activation check failed: %d", err);
        return;
    }
    ESP_LOGI(TAG, "Chat configuration response: code=%d challenge=%d websocket=%d mqtt=%d",
             ota.HasActivationCode(), ota.HasActivationChallenge(),
             ota.HasWebsocketConfig(), ota.HasMqttConfig());
    if (ota.HasActivationChallenge()) {
        chat_activation_pending_ = true;
        if (ota.HasActivationCode() && ota.GetActivationCode() != GetPendingActivationCode()) {
            ShowActivationCode(ota.GetActivationCode());
        }
        err = ota.Activate();
        if (activation_paused_.load(std::memory_order_acquire)) {
            return;
        }
        if (err == ESP_OK) { err = ota.CheckChatConfiguration(); }
    }
    if (ota.HasActivationCode() || ota.HasActivationChallenge()) {
        ESP_LOGI(TAG, "Chat activation still pending; retry remains enabled");
        chat_activation_pending_ = true;
        if (ota.HasActivationCode()) {
            if (ota.GetActivationCode() != GetPendingActivationCode()) {
                ShowActivationCode(ota.GetActivationCode());
            }
        } else {
            board.GetDisplay()->SetStatus(Lang::Strings::ACTIVATION);
        }
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Chat activation failed: %d", err);
        return;
    }
    if (!has_endpoint()) {
        ESP_LOGE(TAG, "Chat OTA configuration unavailable: err=%d websocket=%d mqtt=%d",
                 err, ota.HasWebsocketConfig(), ota.HasMqttConfig());
        return;
    }
    ClearActivationCode();
    chat_activation_pending_ = false;
    chat_config_ready_ = true;
    ESP_LOGI(TAG, "Chat activation confirmed; cached code cleared");
    {
        std::lock_guard<std::mutex> lock(activation_state_mutex_);
        current_version_ = ota.GetCurrentVersion();
    }
    if (ota.HasServerTime()) { board.OnNetworkTimeSynced(); }
    if (device_state_ == kDeviceStateIdle) { ApplyIdleStatusBar(); }
    if (boot_ready_.load(std::memory_order_acquire) && AssistantScreen::IsActive()) {
        StartXiaozhiVoice(AssistantScreen::IsPttHeld());
    }
}

void Application::EnsureProtocolReady() {
    if (!chat_cloud::Available()) {
        ESP_LOGI(TAG, "EnsureProtocolReady skipped: blank cloud endpoints");
        protocol_ready_ = false;
        protocol_.reset();
        return;
    }
    if (protocol_ready_ && protocol_) {
        return;
    }
    // Activation is checked during startup/background retries, never on chat entry.
    if (!chat_config_ready_ || chat_activation_pending_) {
        ESP_LOGI(TAG, "Chat protocol deferred: ready=%d activation_pending=%d",
                 chat_config_ready_.load(), chat_activation_pending_.load());
        activation_kick_.store(true, std::memory_order_release);
        if (HasPendingActivationCode()) {
            RestoreActivationStatus();
        }
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    Settings ws(chat_cloud::WebsocketNamespace());
    Settings mqtt(chat_cloud::MqttNamespace());
    if (!chat_cloud::IsYunxin() && ws.GetString("url").empty() &&
        !mqtt.GetString("endpoint").empty()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else {
        protocol_ = std::make_unique<WebsocketProtocol>();
    }

    const auto generation = voice_generation_.load();
    const auto current_session = [this, generation]() {
        return generation == voice_generation_.load() && AssistantScreen::IsActive() && !StandbyScreen::IsActive();
    };
    protocol_->OnConnected([this, current_session]() {
        if (current_session()) DismissAlert();
    });

    protocol_->OnNetworkError([this, generation, current_session](const std::string& message) {
        if (!current_session()) return;
        {
            std::lock_guard<std::mutex> lock(activation_state_mutex_);
            last_error_message_ = message;
            last_error_generation_ = generation;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this, current_session](std::unique_ptr<AudioStreamPacket> packet) {
        if (current_session() && device_state_ == kDeviceStateSpeaking && chat_cloud::SpeechPlaybackEnabled()) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this]() {
        FirmwareMemoryCheckpoint("chat-channel-open");
        auto& board = Board::GetInstance();
        auto codec = board.GetAudioCodec();
        PowerPolicy::GetInstance().Acquire(PowerNeed::AudioSession);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, current_session]() {
        PowerPolicy::GetInstance().Release(PowerNeed::AudioSession);
        auto after = [this, current_session]() {
            if (!current_session()) return;
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        };
        if (chat_cloud::IsYunxin()) { QueueYunxinTextBoundary(std::move(after)); }
        else { Schedule(std::move(after)); }
    });
    protocol_->OnIncomingJson([this, current_session](const cJSON* root) {
        if (!current_session()) return;
        auto display = Board::GetInstance().GetDisplay();
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this, current_session]() {
                    if (!current_session()) return;
                    aborted_ = false;
                    if (chat_cloud::SpeechPlaybackEnabled() &&
                        (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening)) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                auto after = [this, current_session]() {
                    if (!current_session()) return;
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                };
                if (chat_cloud::IsYunxin()) { QueueYunxinTextBoundary(std::move(after)); }
                else { Schedule(std::move(after)); }
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    if (chat_cloud::IsYunxin()) { ReceiveYunxinText(text->valuestring); }
                    else {
                        Schedule([display, current_session, message = std::string(text->valuestring)]() {
                            if (!current_session()) return;
                            display->SetChatMessage("assistant", message.c_str());
                        });
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                auto after = [display, current_session, message = std::string(text->valuestring)]() {
                    if (!current_session()) return;
                    display->SetChatMessage("user", message.c_str());
                };
                if (chat_cloud::IsYunxin()) { QueueYunxinTextBoundary(std::move(after)); }
                else { Schedule(std::move(after)); }
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, current_session, emotion_str = std::string(emotion->valuestring)]() {
                    if (!current_session()) return;
                    display->SetEmotion(emotion_str.c_str());
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
                    Schedule([this, current_session]() {
                        if (current_session()) Reboot();
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
                Alert(status->valuestring, message->valuestring, emotion->valuestring);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, current_session, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    if (!current_session()) return;
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type");
        }
    });

    FirmwareMemoryCheckpoint("chat-protocol-before-start");
    bool protocol_started = protocol_->Start();
    FirmwareMemoryCheckpoint("chat-protocol-started");
    protocol_ready_ = true;
    if (chat_cloud::IsYunxin() && yunxin_text_timer_handle_ == nullptr) {
    esp_timer_create_args_t yunxin_timer_args = {
        .callback = [](void* arg) { static_cast<Application*>(arg)->QueueYunxinTextUpdate(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "yx_text",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&yunxin_timer_args, &yunxin_text_timer_handle_) != ESP_OK) {
        ESP_LOGW(TAG, "NetEase text timer unavailable; render at sentence boundaries");
    }
    }
    if (chat_cloud::IsYunxin() && yunxin_text_timer_handle_ != nullptr &&
        !esp_timer_is_active(yunxin_text_timer_handle_)) {
        esp_timer_start_periodic(yunxin_text_timer_handle_, 500000);
    }

    std::string version;
    {
        std::lock_guard<std::mutex> lock(activation_state_mutex_);
        version = current_version_;
    }
    if (protocol_started && !version.empty()) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + version;
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }
}

void Application::TeardownProtocol() {
    if (yunxin_text_timer_handle_ != nullptr) { esp_timer_stop(yunxin_text_timer_handle_); }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    if (chat_cloud::IsYunxin()) {
        for (const auto& snapshot : yunxin_text_inbox_.Cancel()) { ApplyYunxinText(snapshot); }
    }
    protocol_ready_ = false;
}

bool Application::EnterContinuousListening() {
    if (!xiaozhi_voice_active_ || !protocol_) {
        ESP_LOGW(TAG, "EnterContinuousListening ignored: session or protocol not ready");
        return false;
    }

    // 会话内永不启用唤醒词（与 Idle/Speaking 状态机策略一致）
    if (audio_running_) {
        audio_service_.EnableWakeWordDetection(false);
    }

    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!EnsureAssistantNetworkReady()) {
            ESP_LOGE(TAG, "EnterContinuousListening: network not ready");
            SetDeviceState(kDeviceStateIdle);
            if (!StandbyScreen::IsActive()) {
                Alert(Lang::Strings::SERVER_NOT_CONNECTED, Lang::Strings::SERVER_NOT_CONNECTED, "cloud_slash");
            }
            return false;
        }
        if (!protocol_->OpenAudioChannel()) {
            // OpenAudioChannel 内 SetError → MAIN_EVENT_ERROR，勿再 Alert
            ESP_LOGE(TAG, "EnterContinuousListening: OpenAudioChannel failed");
            SetDeviceState(kDeviceStateIdle);
            return false;
        }
    }

    SetListeningMode(chat_cloud::UsesXiaozhiProtocol() ? kListeningModeManualStop :
                     (aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime));
    return true;
}

void Application::StartXiaozhiVoice(bool enter_listening) {
    const auto generation = voice_generation_.load();
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this, enter_listening, generation]() { StartXiaozhiVoiceOnTask(enter_listening, generation); })) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_PREPARE_VOICE);
        }
        return;
    }
    StartXiaozhiVoiceOnTask(enter_listening, generation);
}

void Application::StartXiaozhiVoiceOnTask(bool enter_listening, uint32_t generation) {
    const auto still_needed = [this, generation]() {
        return generation == voice_generation_.load() && AssistantScreen::IsActive() && !StandbyScreen::IsActive();
    };
    if (!still_needed()) return;
    if (enter_listening && RejectUnactivatedListening()) return;
    PowerNeedHold request(PowerNeed::ForegroundNetwork);
    if (xiaozhi_voice_active_ && voice_session_generation_ != generation) StopXiaozhiVoiceOnTask();
    if (Board::GetInstance().GetNetworkErrorText(nullptr)) {
        auto notify = [](void*) {
            if (!AssistantScreen::IsActive()) return;
            if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) ScreenShowToast(hint);
        };
        if (!ScreenLvAsyncUrgent(notify)) ScreenLvAsync(notify);
        return;
    }
    if (!chat_cloud::Available()) {
        ESP_LOGD(TAG, "StartXiaozhiVoice skipped: blank cloud endpoints");
        ClearActivationCode();
        StopXiaozhiVoiceOnTask();
        return;
    }

    // SoftAP 配网无互联网：勿切 Connecting / 空等 EnsureNetworkReady
    if (Board::GetInstance().IsWifiConfigMode()) {
        ESP_LOGW(TAG, "StartXiaozhiVoice skipped: wifi config mode");
        // 与 EnterWifiConfigMode 一致：状态「配网模式」，勿套「错误」
        Alert(Lang::Strings::WIFI_CONFIG_MODE, Lang::Strings::NEED_WIFI_CFG, "gear");
        return;
    }

    // 开机未完成：可进页，但不建协议；记下意图，Start() 收尾后自动兑现
    if (!boot_ready_.load(std::memory_order_acquire)) {
        pending_xiaozhi_voice_.store(true, std::memory_order_release);
        if (enter_listening) {
            pending_xiaozhi_listen_.store(true, std::memory_order_release);
        }
        ESP_LOGI(TAG, "StartXiaozhiVoice deferred until boot ready (listen=%d)",
                 enter_listening ? 1 : 0);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
        display->SetChatMessage("system", Lang::Strings::VOICE_STARTING_NET);
        return;
    }

    if (xiaozhi_voice_active_ && !enter_listening) {
        // 待机唤醒后通道常已断：补等网 + 预开，与进页一致
        if (!still_needed()) {
            return;
        }
        if (protocol_ && !protocol_->IsAudioChannelOpened()) {
            ESP_LOGI(TAG, "Xiaozhi voice: re-preload channel after standby/idle drop");
            SetDeviceState(kDeviceStateConnecting);
            if (!EnsureAssistantNetworkReady(still_needed)) {
                if (!still_needed()) {
                    StopXiaozhiVoiceOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice: re-preload network not ready");
                SetDeviceState(kDeviceStateIdle);
                if (!StandbyScreen::IsActive()) {
                    ApplyIdleStatusBar();
                    Alert(Lang::Strings::SERVER_NOT_CONNECTED, Lang::Strings::SERVER_NOT_CONNECTED, "cloud_slash");
                }
                return;
            }
            if (!protocol_->OpenAudioChannel()) {
                if (!still_needed()) {
                    StopXiaozhiVoiceOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice: re-preload OpenAudioChannel failed");
                SetDeviceState(kDeviceStateIdle);
                ApplyIdleStatusBar();
                return;
            }
            if (!still_needed()) {
                StopXiaozhiVoiceOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
        }
        if (still_needed() && pending_xiaozhi_listen_.exchange(false, std::memory_order_acq_rel) &&
            AssistantScreen::IsActive() && AssistantScreen::IsPttHeld() &&
            protocol_ && protocol_->IsAudioChannelOpened()) {
            StartListening();
        }
        return;
    }

    // LOADED 的 Schedule 可能晚于 UNLOADED：离页后不再拉起会话，避免孤儿协议/音频
    if (!still_needed()) {
        ESP_LOGW(TAG, "StartXiaozhiVoice skipped: assistant not active");
        return;
    }

    if (!xiaozhi_voice_active_) {
        if (!MetalioAudio_ModeReady()) {
            // 复位与应答等待放在后台，避免阻塞主事件循环。
            if (audio_mode_wait_pending_.exchange(true)) return;
            struct ModeWaitContext {
                Application* app; // 全局应用实例，生命周期覆盖后台任务
                uint32_t generation; // 启动请求所属代号
            };
            auto* pending = new (std::nothrow) ModeWaitContext{this, generation};
            if (pending == nullptr) {
                audio_mode_wait_pending_.store(false);
                if (still_needed()) Alert(Lang::Strings::ERROR, Lang::Strings::AUDIO_INIT_FAILED, "circle_xmark");
                return;
            }
            const auto started = xTaskCreatePinnedToCoreWithCaps([](void* context) {
                auto* pending = static_cast<ModeWaitContext*>(context);
                auto* app = pending->app;
                const auto generation = pending->generation;
                delete pending;
                bool ready = false;
                const auto still_needed = [app, generation] {
                    return generation == app->voice_generation_.load() &&
                           AssistantScreen::IsActive() && !StandbyScreen::IsActive();
                };
                if (still_needed()) {
                    PowerNeedHold request(PowerNeed::ForegroundNetwork);
                    PowerNeedHold power(PowerNeed::LocalMic);
                    ready = MetalioAudio_WaitModeReady(kMetalioAudioInitWaitMs, still_needed);
                }
                if (!app->Schedule([app, ready, generation] { app->OnAudioModeReady(generation, ready); })) {
                    app->audio_mode_wait_pending_.store(false);
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_PREPARE_VOICE);
                }
                vTaskDeleteWithCaps(nullptr);
            }, "audio_ready", 4096, pending, 5, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (started != pdPASS) {
                delete pending;
                audio_mode_wait_pending_.store(false);
                if (still_needed()) Alert(Lang::Strings::ERROR, Lang::Strings::AUDIO_INIT_FAILED, "circle_xmark");
            }
            return;
        }
        ESP_LOGI(TAG, "Starting Xiaozhi voice session (%s)",
                 enter_listening ? "hold-through listen" : "PTT: prepare only");

        if (!chat_cloud::Available() || !chat_config_ready_ || chat_activation_pending_) {
            EnsureProtocolReady();
            return;
        }
        // 先申请音频 DMA，并保持外部时钟源供电，再启动网络协议。
        PowerNeedHold audio_start_power(PowerNeed::LocalMic);
        if (!AcquireAudioService(AudioClient::Assistant, still_needed)) return;
        EnsureProtocolReady();
        if (!protocol_) {
            ReleaseAudioService(AudioClient::Assistant);
            return;
        }
        EnsureVoiceModelsReady();

        // Ensure* 可能较慢；期间离页则拆协议并停音频，与 StopXiaozhiVoice 一致归还占用
        if (!still_needed()) {
            ESP_LOGW(TAG, "StartXiaozhiVoice aborted after Ensure*: left screen");
            TeardownProtocol();
            ReleaseAudioService(AudioClient::Assistant);
            return;
        }

        if (!protocol_) {
            ESP_LOGW(TAG, "StartXiaozhiVoice aborted: no protocol from OTA config");
            ReleaseAudioService(AudioClient::Assistant);
            return;
        }

        xiaozhi_voice_active_ = true;
        voice_session_generation_ = generation;

        // 会话内永不启用唤醒词；非 hold-through 时等 BOOT 按下再开语音处理
        if (audio_running_) {
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.EnableVoiceProcessing(false);
        }
    }

    // 长按进页：开通道后直接 manual 聆听，不落 Idle/待命
    if (enter_listening) {
        if (!protocol_) {
            ESP_LOGW(TAG, "Xiaozhi voice hold-through: no protocol");
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
            return;
        }
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!EnsureAssistantNetworkReady(still_needed)) {
                if (!still_needed()) {
                    StopXiaozhiVoiceOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice hold-through: network not ready");
                SetDeviceState(kDeviceStateIdle);
                if (!StandbyScreen::IsActive()) {
                    ApplyIdleStatusBar();
                    Alert(Lang::Strings::SERVER_NOT_CONNECTED, Lang::Strings::SERVER_NOT_CONNECTED, "cloud_slash");
                }
                return;
            }
            if (!protocol_->OpenAudioChannel()) {
                if (!still_needed()) {
                    StopXiaozhiVoiceOnTask();
                    return;
                }
                // SetError → MAIN_EVENT_ERROR 已提示，勿双 Alert
                ESP_LOGW(TAG, "Xiaozhi voice hold-through: OpenAudioChannel failed");
                SetDeviceState(kDeviceStateIdle);
                ApplyIdleStatusBar();
                return;
            }
        }
        // OpenAudioChannel 阻塞期间可能已松手或离页；Connecting 时 StopListening 会忽略
        if (!still_needed()) {
            ESP_LOGI(TAG, "hold-through aborted: left screen during/after channel open");
            StopXiaozhiVoiceOnTask();
            return;
        }
        if (!BootKey_IsHeld()) {
            ESP_LOGI(TAG, "hold-through aborted: BOOT released before listen -> Idle");
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
            AssistantScreen::SyncPttOverlay();
            return;
        }
        SetListeningMode(kListeningModeManualStop);
        return;
    }

    // 预开 WebSocket，缩短首按延迟；失败不退出会话，按下时 StartListening 会再试
    if (protocol_ && !protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!EnsureAssistantNetworkReady(still_needed)) {
            ESP_LOGW(TAG, "Xiaozhi voice: preload network not ready; retry on PTT");
            if (!still_needed()) {
                if (!StandbyScreen::IsActive()) {
                    StopXiaozhiVoiceOnTask();
                } else {
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
            Alert(Lang::Strings::SERVER_NOT_CONNECTED, Lang::Strings::SERVER_NOT_CONNECTED, "cloud_slash");
        } else if (!protocol_->OpenAudioChannel()) {
            ESP_LOGW(TAG, "Xiaozhi voice: preload OpenAudioChannel failed; retry on PTT");
            if (!still_needed()) {
                StopXiaozhiVoiceOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
        } else {
            if (!still_needed()) {
                StopXiaozhiVoiceOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            ApplyIdleStatusBar();
        }
    } else {
        if (!still_needed()) {
            StopXiaozhiVoiceOnTask();
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        ApplyIdleStatusBar();
    }

    // 预连接期间的长按在通道就绪后兑现，松手或离页不补录音。
    if (still_needed() && pending_xiaozhi_listen_.exchange(false, std::memory_order_acq_rel) &&
        AssistantScreen::IsActive() && AssistantScreen::IsPttHeld() &&
        protocol_ && protocol_->IsAudioChannelOpened()) {
        StartListening();
    }
}

void Application::OnAudioModeReady(uint32_t generation, bool ready) {
    audio_mode_wait_pending_.store(false);
    if (!AssistantScreen::IsActive() || StandbyScreen::IsActive()) return;
    if (generation != voice_generation_.load()) {
        // 旧等待已经取消，当前页面仍需恢复连接，不能沿用旧失败或录音意图。
        StartXiaozhiVoice(false);
    } else if (ready) {
        StartXiaozhiVoice(AssistantScreen::IsPttHeld());
    } else {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, Lang::Strings::AUDIO_INIT_FAILED, "circle_xmark");
    }
}

void Application::StopXiaozhiVoice() {
    voice_generation_.fetch_add(1);
    pending_xiaozhi_voice_.store(false, std::memory_order_release);
    pending_xiaozhi_listen_.store(false, std::memory_order_release);
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this]() {
            if (voice_session_generation_ != voice_generation_.load()) StopXiaozhiVoiceOnTask();
        })) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_VOICE);
        }
        return;
    }
    StopXiaozhiVoiceOnTask();
}

void Application::StopXiaozhiVoiceOnTask() {
    if (!xiaozhi_voice_active_) {
        return;
    }
    ESP_LOGI(TAG, "Stopping Xiaozhi voice session");
    xiaozhi_voice_active_ = false;
    // 覆盖音频释放到协议关闭之间的收尾阶段，待机任务不能在此期间进入浅睡。
    PowerNeedHold closing(PowerNeed::ForegroundNetwork);

    // Only the last audio consumer releases tasks and I2S DMA.
    ReleaseAudioService(AudioClient::Assistant);
    TeardownProtocol();

    // 须走 SetDeviceState，否则 PowerPolicy 仍停在 Connecting → NetActive 不断网
    SetDeviceState(kDeviceStateIdle);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        Alert(Lang::Strings::LOADING_ASSETS, Lang::Strings::PLEASE_WAIT, "cloud_arrow_down");
        
        // Give UI time before entering upgrade state
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark");
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const bool background = boot_ready_.load(std::memory_order_acquire);
    if (!background) ClearActivationCode();

    auto& board = Board::GetInstance();
    // 开机未连上时跳过；后台联网后先补版本检测，再检查激活。
    if (!board.IsNetworkReady()) {
        ESP_LOGW(TAG, "skip CheckNewVersion: network not ready");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    // Cloudzao 空白开源版必须无条件跳过 OTA / 激活检测；即使旧 NVS 里残留 ota_url 也不能继续请求。
    if (!chat_cloud::UsesXiaozhiProtocol() && !api::HasDefaultOtaUrl()) {
        ESP_LOGW(TAG, "skip CheckNewVersion: Cloudzao blank endpoints");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    if (ota.GetCheckVersionUrl().length() < 10) {
        ESP_LOGW(TAG, "skip CheckNewVersion: OTA URL not configured");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    auto cancelled_for_standby = [this]() {
        if (!StandbyScreen::IsActive()) return false;
        boot_version_checked_ = false; // retry after wake/connectivity returns
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return true;
    };
    auto interrupted = [this]() {
        if (!activation_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        // 热点或 U 盘没有互联网：这次不算查完，恢复网络后再查
        ESP_LOGI(TAG, "version check interrupted");
        boot_version_checked_ = false;
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return true;
    };
    while (true) {
        if (cancelled_for_standby() || interrupted()) return;
        if (!background) SetDeviceState(kDeviceStateActivating);
        if (cancelled_for_standby() || interrupted()) return;
        if (!background && device_state_ == kDeviceStateIdle) {
            break;
        }
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (cancelled_for_standby() || interrupted()) return;
        if (err != ESP_OK) {
            // Ota owns the bounded recovery sequence. Do not multiply it by
            // another exponential retry loop and block boot for many minutes.
            ESP_LOGE(TAG, "Version check recovery exhausted, code=%d", int(err));
            Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_ERROR, "cloud_slash");
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            return;
        }

        if (ota.HasNewVersion()) {
            const std::string& new_ver = ota.GetFirmwareVersion();
            if (OtaIgnore::IsIgnored(new_ver)) {
                ESP_LOGI(TAG, "firmware %s ignored in NVS; skip prompt", new_ver.c_str());
            } else {
                const auto choice = OtaConfirmDialog::ShowBlocking(
                    ota.GetCurrentVersion().c_str(), new_ver.c_str(), OtaConfirmDialog::Mode::Boot);
                if (choice == OtaConfirmDialog::Choice::Failed) {
                    ESP_LOGW(TAG, "OTA confirmation unavailable; retry before continuing boot");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                if (choice == OtaConfirmDialog::Choice::Upgrade) {
                    if (background) {
                        const auto url = ota.GetFirmwareUrl();
                        const auto current = ota.GetCurrentVersion();
                        if (!Schedule([this, url, current, new_ver]() {
                            UpgradeFirmwareUrl(url, current, new_ver);
                        })) ESP_LOGE(TAG, "firmware upgrade dispatch failed");
                        return;
                    }
                    if (UpgradeFirmware(ota)) {
                        return;  // reboot
                    }
                    // upgrade failed → fall through existing boot path
                } else if (choice == OtaConfirmDialog::Choice::IgnorePersist) {
                    OtaIgnore::Set(new_ver);
                }
                // RemindLater / ignore-after-set / upgrade-fail: same fall through as no new version
            }
        }

        ota.MarkCurrentVersionValid();
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        break;
    }
}

bool Application::TrySdCardFirmwareUpgrade() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }

    struct stat st {};
    if (stat(kOtaSdFirmwarePath, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return false;
    }

    ESP_LOGI(TAG, "SD firmware found: %s (%ld bytes)", kOtaSdFirmwarePath,
             static_cast<long>(st.st_size));

    const esp_app_desc_t* app_desc = esp_app_get_description();
    const std::string current_version =
        (app_desc != nullptr && app_desc->version[0] != '\0') ? app_desc->version : "—";

    // Sd 模式：立刻升级 / 取消；文案仅「SD卡发现新固件 + 当前版本」
    const auto choice = OtaConfirmDialog::ShowBlocking(
        current_version.c_str(), "", OtaConfirmDialog::Mode::Sd);
    if (choice != OtaConfirmDialog::Choice::Upgrade) {
        ESP_LOGI(TAG, "SD firmware upgrade cancelled; keep %s for next boot", kOtaSdFirmwarePath);
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    OtaTouchPause touch_pause;
    SetDeviceState(kDeviceStateUpgrading);
    // 进度页不传最新版本（空串隐藏「最新版本」行）
    if (!OtaUpgradeScreen::Show(current_version.c_str(), "")) {
        ESP_LOGW(TAG, "SD OTA upgrade UI show incomplete; continuing with keys locked");
    }
    OtaUpgradeScreen::SetProgress(0);
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download");
    vTaskDelay(pdMS_TO_TICKS(800));

    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    StopAudioServiceIfRunning();
    vTaskDelay(pdMS_TO_TICKS(500));

    Ota ota;
    const bool ok = ota.StartUpgradeFromFile(kOtaSdFirmwarePath, [](int progress, size_t /*speed*/) {
        OtaUpgradeScreen::SetProgress(progress);
    });
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);

    auto remove_sd_fw = []() {
        if (unlink(kOtaSdFirmwarePath) != 0) {
            ESP_LOGW(TAG, "failed to remove %s: errno=%d", kOtaSdFirmwarePath, errno);
        } else {
            ESP_LOGI(TAG, "removed %s", kOtaSdFirmwarePath);
        }
    };

    if (!ok) {
        ESP_LOGE(TAG, "SD firmware upgrade failed");
        OtaUpgradeScreen::SetHint(Lang::Strings::UPGRADE_FAILED);
        vTaskDelay(pdMS_TO_TICKS(3000));
        OtaUpgradeScreen::Dismiss();
        remove_sd_fw();
        if (xiaozhi_voice_active_) {
            EnsureAudioServiceRunning();
        }
        return false;
    }

    OtaIgnore::Clear();
    ESP_LOGI(TAG, "SD firmware upgrade successful, rebooting...");
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint(Lang::Strings::OTA_SUCCESS_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(1200));
    remove_sd_fw();
    Reboot();
    return true;
}

void Application::ShowActivationCode(const std::string& code) {
    {
        std::lock_guard<std::mutex> lock(activation_state_mutex_);
        activation_code_ = code;
    }
    // 只在顶部状态栏显示验证码，不语音播报；日志里不打印 host 文本，避免任何云域名残留。
    char status[64];
    std::snprintf(status, sizeof(status), Lang::Strings::ACTIVATION_CODE_FMT, code.c_str());

    ESP_LOGW(TAG, "Activation [%s]", status);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
}

bool Application::HasPendingActivationCode() const {
    std::lock_guard<std::mutex> lock(activation_state_mutex_);
    return !activation_code_.empty();
}

std::string Application::GetPendingActivationCode() const {
    std::lock_guard<std::mutex> lock(activation_state_mutex_);
    return activation_code_;
}

void Application::ClearActivationCode() {
    std::lock_guard<std::mutex> lock(activation_state_mutex_);
    activation_code_.clear();
}

void Application::RestoreActivationStatus() {
    const auto code = GetPendingActivationCode();
    if (code.empty()) return;
    char status[64];
    std::snprintf(status, sizeof(status), Lang::Strings::ACTIVATION_CODE_FMT, code.c_str());
    Board::GetInstance().GetDisplay()->SetStatus(status);
}

void Application::WaitWhileActivationPaused() {
    while (activation_paused_.load(std::memory_order_acquire) &&
           device_state_ == kDeviceStateActivating) {
        if (StandbyScreen::IsActive()) return;
        // 暂停期间吞掉 kick，避免一恢复就连打 Activate
        activation_kick_.store(false, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool Application::DelayMsInterruptible(int ms) {
    constexpr int kStepMs = 100;
    for (int elapsed = 0; elapsed < ms; elapsed += kStepMs) {
        if (device_state_ == kDeviceStateIdle) {
            return false;
        }
        if (activation_paused_.load(std::memory_order_acquire)) {
            WaitWhileActivationPaused();
            if (device_state_ == kDeviceStateIdle) {
                return false;
            }
            // 暂停结束：本段延迟已实质耗尽，交回循环决定是否再 Activate
            return true;
        }
        if (activation_kick_.exchange(false, std::memory_order_acq_rel)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
    }
    if (device_state_ == kDeviceStateIdle) {
        return false;
    }
    return activation_kick_.exchange(false, std::memory_order_acq_rel);
}

void Application::PauseActivationPolling() {
    std::lock_guard<std::mutex> lock(activation_poll_mutex_);
    ++activation_pause_count_;
    activation_paused_.store(true, std::memory_order_release);
}

void Application::ResumeActivationPolling() {
    std::lock_guard<std::mutex> lock(activation_poll_mutex_);
    if (activation_pause_count_) --activation_pause_count_;
    activation_paused_.store(activation_pause_count_ != 0, std::memory_order_release);
    if (!activation_pause_count_) activation_kick_.store(true, std::memory_order_release);
}

void Application::ResumeActivationAfterStandby() {
    Schedule([this]() {
        if (device_state_ != kDeviceStateActivating && chat_config_ready_) {
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(!HasPendingActivationCode() ? Lang::Strings::CHECKING_NEW_VERSION
                                                    : Lang::Strings::ACTIVATION);
        display->SetEmotion("neutral");
        RestoreActivationStatus();
        // 上传仍暂停时只恢复 UI，不 kick 轮询
        if (!activation_paused_.load(std::memory_order_acquire)) {
            activation_kick_.store(true, std::memory_order_release);
        }
        ESP_LOGI(TAG, "resume activation after standby LP (paused=%d)",
                 activation_paused_.load(std::memory_order_acquire) ? 1 : 0);
    });
}

void Application::Alert(const char* status, const char* message, const char* emotion) {
    ESP_LOGW(TAG, "Alert raised");
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
}

void Application::ApplyIdleStatusBar() {
    auto display = Board::GetInstance().GetDisplay();
    if (HasPendingActivationCode()) {
        RestoreActivationStatus();
        return;
    }
    // 固定文案页：不刷时钟。百问页已声明 kClock，会话 Idle 也走时钟，勿显示「待命」
    if (!display->AllowsIdleStatusClock()) {
        const char* text = display->GetIdleStatusFixedText();
        display->SetStatus((text != nullptr && text[0] != '\0') ? text : Lang::Strings::STANDBY);
        return;
    }

    // 首页 / 百问 Idle：直接显示时钟
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= (2025 - 1900)) {
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
        display->SetStatus(time_str);
    } else {
        display->SetStatus("--:--");
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        ApplyIdleStatusBar();
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

bool Application::RejectUnactivatedListening() {
    if (!AssistantScreen::IsActive() || StandbyScreen::IsActive() ||
        (!chat_activation_pending_.load() && !HasPendingActivationCode())) return false;
    auto notify = [](void*) {
        auto& app = Application::GetInstance();
        if (AssistantScreen::IsActive() && !StandbyScreen::IsActive() &&
            (app.chat_activation_pending_.load() || app.HasPendingActivationCode())) {
            ScreenShowToast("请先激活设备");
        }
    };
    if (!ScreenLvAsyncUrgent(notify)) ScreenLvAsync(notify);
    return true;
}

void Application::StartListening() {
    if (RejectUnactivatedListening()) return;
    const auto generation = voice_generation_.load();
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this, generation]() {
            if (generation == voice_generation_.load()) StartListening();
        })) ESP_LOGE(TAG, "listen start dispatch failed");
        return;
    }
    const auto still_needed = [this, generation]() {
        return generation == voice_generation_.load() && AssistantScreen::IsActive() && !StandbyScreen::IsActive();
    };
    if (!still_needed()) return;
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        // 原工程配网态长按 BOOT 会进本地录音环回；产品不要此行为
        ESP_LOGI(TAG, "StartListening ignored in wifi configuring (no audio testing)");
        return;
    }

    if (!boot_ready_.load(std::memory_order_acquire)) {
        pending_xiaozhi_voice_.store(true, std::memory_order_release);
        pending_xiaozhi_listen_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "StartListening deferred until boot ready");
        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PLEASE_WAIT);
        return;
    }

    if (!xiaozhi_voice_active_ || !protocol_) {
        if (!chat_config_loading_ && AssistantScreen::IsActive()) {
            Schedule([this, generation, still_needed]() {
                if (!still_needed()) return;
                StartXiaozhiVoiceOnTask(false, generation);
                if (still_needed() && xiaozhi_voice_active_ && protocol_ && AssistantScreen::IsPttHeld()) {
                    StartListening();
                }
            });
        }
        return;
    }
    
    if (device_state_ == kDeviceStateConnecting && AssistantScreen::IsActive()) {
        pending_xiaozhi_listen_.store(true, std::memory_order_release);
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this, still_needed]() {
            if (!still_needed() || !xiaozhi_voice_active_ || !protocol_) {
                return;
            }
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                ESP_LOGI(TAG, "StartListening: audio channel not open, EnsureNetwork+Open");
                if (!EnsureAssistantNetworkReady(still_needed)) {
                    if (!still_needed()) {
                        StopXiaozhiVoiceOnTask();
                        return;
                    }
                    ESP_LOGE(TAG, "StartListening: network not ready");
                    SetDeviceState(kDeviceStateIdle);
                    if (!StandbyScreen::IsActive()) {
                        ApplyIdleStatusBar();
                        Alert(Lang::Strings::SERVER_NOT_CONNECTED, Lang::Strings::SERVER_NOT_CONNECTED, "cloud_slash");
                    }
                    return;
                }
                if (!protocol_->OpenAudioChannel()) {
                    if (!still_needed()) {
                        StopXiaozhiVoiceOnTask();
                        return;
                    }
                    ESP_LOGE(TAG, "StartListening: OpenAudioChannel failed");
                    SetDeviceState(kDeviceStateIdle);
                    ApplyIdleStatusBar();
                    return;
                }
            } else {
                ESP_LOGI(TAG, "StartListening: reuse open audio channel");
            }

            if (!still_needed()) {
                StopXiaozhiVoiceOnTask();
                return;
            }

            // 百问页长按臂听：开通道期间已松手则回待命（BOOT ∪ 屏触 PTT）
            if (AssistantScreen::IsActive() && !AssistantScreen::IsPttHeld()) {
                ESP_LOGI(TAG, "StartListening aborted: PTT released before listen");
                SetDeviceState(kDeviceStateIdle);
                ApplyIdleStatusBar();
                AssistantScreen::SyncPttOverlay();
                return;
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this, still_needed]() {
            if (!still_needed() || !xiaozhi_voice_active_) {
                return;
            }
            if (device_state_ != kDeviceStateSpeaking) {
                return;
            }
            AbortSpeaking(kAbortReasonNone);
            // 长按打断并开听；若已松手则只落到待命，不进聆听中
            if (AssistantScreen::IsActive() && !AssistantScreen::IsPttHeld()) {
                ESP_LOGI(TAG, "StartListening from speaking aborted: PTT released -> Idle");
                if (audio_running_) {
                    audio_service_.ResetDecoder();
                }
                SetDeviceState(kDeviceStateIdle);
                AssistantScreen::SyncPttOverlay();
                return;
            }
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    pending_xiaozhi_listen_.store(false, std::memory_order_release);
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this]() { StopListening(); })) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
        }
        return;
    }
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->SendStopListening();
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeakingToIdle() {
    // 仅百问会话 + Speaking：短按打断 → 待命。不 StartListening，避免闪「聆听中」。
    if (!xiaozhi_voice_active_ || device_state_ != kDeviceStateSpeaking) {
        return;
    }

    Schedule([this]() {
        if (!xiaozhi_voice_active_ || device_state_ != kDeviceStateSpeaking) {
            return;
        }
        AbortSpeaking(kAbortReasonNone);
        if (audio_running_) {
            audio_service_.ResetDecoder();
        }
        SetDeviceState(kDeviceStateIdle);
    });
}

void Application::FlushPendingXiaozhiVoice() {
    if (!pending_xiaozhi_voice_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const bool want_listen = pending_xiaozhi_listen_.exchange(false, std::memory_order_acq_rel);
    if (!AssistantScreen::IsActive()) {
        ESP_LOGI(TAG, "pending xiaozhi voice dropped: left assistant");
        return;
    }
    // hold-through：仍按住则直接聆听；已松手则只准备会话，无需重进页
    const bool enter_listening = want_listen && BootKey_IsHeld();
    ESP_LOGI(TAG, "boot ready -> resume xiaozhi voice (listen=%d held=%d)",
             enter_listening ? 1 : 0, BootKey_IsHeld() ? 1 : 0);
    StartXiaozhiVoice(enter_listening);
}

void Application::BootStandbyCheckpoint() {
    boot_work_active_.store(true, std::memory_order_release);
    if (StandbyScreen::IsActive()) {
        boot_work_active_.store(false, std::memory_order_release);
        ESP_LOGI(TAG, "boot paused for standby (SD/network initialization deferred)");
        while (StandbyScreen::IsActive()) vTaskDelay(pdMS_TO_TICKS(20));
        boot_work_active_.store(true, std::memory_order_release);
    }
}

void Application::Start() {
    BootStandbyCheckpoint();
    chat_cloud::InitializeFromNvs();
    // UI 语言：NVS ui/language，无则 LANG_DEFAULT_CODE（跟 CONFIG_LANGUAGE_*）
    Lang::InitFromNvs();

    // SD 运行日志（默认开；有卡写 metalio/e-ink/logs/metalio_logs.txt）
    // 挂载且开启时：Flash coredump 覆盖写入 metalio_coredump.elf 后擦槽
    SdLogWriter_Start();
    // The 470 SD logger exports crashes on its existing internal-stack worker.

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // fontpack mmap 须在内部 DRAM 栈任务上完成；开机预热 UI 25@2，避免各页首绘空字体
    fontpack_lv_ensure_ready();
    (void)fontpack_lv_font_ui();

    // A2UI 图片会话缓存：有 SD 则用 /sdcard/metalio/e-ink/a2ui_cache（开机清空）；无卡则仅 HTTP
    {
        esp_err_t cache_err = a2ui_img_cache_init();
        if (cache_err != ESP_OK) {
            ESP_LOGW(TAG, "a2ui_img_cache_init: %s (HTTP-only images)", esp_err_to_name(cache_err));
        }
    }

    // 百问AI 会话 JSON：有 SD 则 /sdcard/metalio/e-ink/chat_log（开机清空）；无卡不记历史
    {
        esp_err_t chat_err = assistant_chat_store_init();
        if (chat_err != ESP_OK) {
            ESP_LOGW(TAG, "assistant_chat_store_init: %s (RAM-only chat)", esp_err_to_name(chat_err));
        }
    }

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // 主任务仅分发事件；业务回调由独立的内部 RAM 栈工作任务处理。
    if (xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 3072, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "main dispatcher allocation failed");
        display->SetStatus(Lang::Strings::ERROR);
        return;
    }

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // SD 已在板级构造时挂载；UI/主循环就绪后立刻提示，勿等联网
    Ota ota;
    ota.MarkCurrentVersionValid();
    BootStandbyCheckpoint();
    TrySdCardFirmwareUpgrade();
    BootStandbyCheckpoint();

    // 书库预热须在 StartNetwork 之前（仅创建后台任务）。
    reader::book_home_snapshot::HydrateFromNvs();
    reader::book_library_warmup::RequestBootWarmup();

    /* 有已存 WiFi：等保网时长；失败不 SoftAP。无 SSID 跳过联网，到设置→网络机上配网 */
    board.StartNetwork();
    BootStandbyCheckpoint();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // OTA URL：NVS(wifi/ota_url) 为空则写入 cloudzao 默认（开源版该串为空则跳过）
    {
        Settings settings("wifi", true);
        std::string ota_url = settings.GetString("ota_url");
        if (!api::HasDefaultOtaUrl()) {
            settings.EraseKey("ota_url");
            ClearActivationCode();
            ESP_LOGI(TAG, "blank cloud endpoints: cleared stale ota_url and activation state");
        } else if (ota_url.empty()) {
            ota_url = api::kDefaultOtaUrl;
            settings.SetString("ota_url", ota_url);
            ESP_LOGD(TAG, "OTA URL empty in NVS, wrote default");
        } else {
            ESP_LOGD(TAG, "OTA URL from NVS");
        }
    }

    // Check for new assets version
    CheckAssetsVersion();
    BootStandbyCheckpoint();

    // Firmware version and AI activation use separate requests, in that order.
    boot_version_checked_ = board.IsNetworkReady() && !board.IsWifiConfigMode();
    CheckNewVersion(ota);
    BootStandbyCheckpoint();
    if (boot_version_checked_) { CheckChatActivation(); }
    BootStandbyCheckpoint();

    has_server_time_ = ota.HasServerTime();
    {
        std::lock_guard<std::mutex> lock(activation_state_mutex_);
        current_version_ = ota.GetCurrentVersion();
    }

    // 联网且拿到 server_time 后，通知板级回写 RTC（如 PCF8563）
    if (has_server_time_) {
        board.OnNetworkTimeSynced();
    }

    // MCP 工具可提前注册；真正连协议在进入 AI 聊天页时
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // 开机收尾只清理无调用方持有的临时音频，与申请/归还共用同一把锁。
    if (!AssistantScreen::IsActive() && !pending_xiaozhi_voice_.load(std::memory_order_acquire)) {
        std::lock_guard<std::recursive_mutex> audio_lock(audio_service_mutex_);
        if (!audio_clients_) {
            StopAudioServiceIfRunning();
        }
    }

    SetDeviceState(kDeviceStateIdle);
    // Core readiness must not wait for optional weather/checklist HTTP requests.
    boot_ready_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Boot core ready; activation polling and PTT enabled");
    FirmwareMemoryCheckpoint("boot-complete");
    FlushPendingXiaozhiVoice();

    ESP_LOGI(TAG, "Boot background warmup begin");
    BootStandbyCheckpoint();
    // 天气：每天开机拉一次，写入 NVS；当天后续走缓存
    StandbyScreen::EnsureWeatherCached();
    // 阅读快照：显示初始化 / StartNetwork 前已灌；此处幂等补灌
    reader::book_home_snapshot::HydrateFromNvs();
    // 开机未连上：勿 EnsureNetworkReady 再空等（会硬占网续扫）
    // 配网 SoftAP / 4G 未真正上网时 IsNetworkReady=false，一律跳过联网任务
    if (board.IsNetworkReady() && !board.IsWifiConfigMode()) {
        if (api::HasHost()) {
            TaskScreen::EnsureCacheSynced();
            // sync 仅 POST：等 warmup 缓存就绪后组包，不再扫盘 Peek
            reader::book_progress_sync::RequestBootReport();
            device_wifi_location::RequestBootReport();
        } else {
            ESP_LOGI(TAG, "skip boot checklist/book sync/wifi-loc: cloud endpoints blank");
        }
    } else {
        ESP_LOGW(TAG, "skip boot checklist/book sync/wifi-loc: network not ready");
    }

    ESP_LOGI(TAG, "Boot background warmup complete");
    boot_work_active_.store(false, std::memory_order_release);
}

void Application::ApplyYunxinText(const YunxinTextInbox::Snapshot& snapshot) {
    if (!yunxin_text_inbox_.IsCurrent(snapshot)) { return; }
    try {
        if (!snapshot.text.empty()) { AssistantScreen::AddYunxinSpeech(snapshot.text.c_str()); }
        if (snapshot.finish) { AssistantScreen::FinishYunxinSpeech(); }
        else { AssistantScreen::FlushYunxinSpeech(); }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "NetEase text skipped: PSRAM exhausted");
    }
    yunxin_text_inbox_.Applied(snapshot);
    if (snapshot.after) snapshot.after();
}

void Application::ReceiveYunxinText(const char* text) try {
    yunxin_text_inbox_.Push(text, [this](YunxinTextInbox::Snapshot snapshot) {
        return Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
    });
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "NetEase text inbox full: PSRAM exhausted");
}

void Application::QueueYunxinTextUpdate() try {
    yunxin_text_inbox_.Update([this](YunxinTextInbox::Snapshot snapshot) {
        return Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
    });
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "NetEase text update deferred: PSRAM exhausted");
}

void Application::QueueYunxinTextBoundary(std::function<void()> after) try {
    yunxin_text_inbox_.Finish([this](YunxinTextInbox::Snapshot snapshot) {
        return Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
    }, after);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "NetEase text boundary dropped: PSRAM exhausted");
    Schedule(std::move(after));
}

void Application::DispatchVoiceEvents() {
    bool expected = false;
    if (!voice_events_queued_.compare_exchange_strong(expected, true)) return;
    if (!Schedule([this]() {
        const auto bits = voice_events_.exchange(0, std::memory_order_acq_rel);
        // 满队列中的停止意图仍需交付；重进页面或再次按住后不执行旧停止意图。
        if ((bits & MAIN_EVENT_STOP_VOICE) && voice_session_generation_ != voice_generation_.load()) {
            StopXiaozhiVoiceOnTask();
        }
        if ((bits & MAIN_EVENT_PREPARE_VOICE) && AssistantScreen::IsActive() && !StandbyScreen::IsActive()) {
            StartXiaozhiVoice(false);
        }
        if ((bits & MAIN_EVENT_STOP_LISTENING) && !AssistantScreen::IsPttHeld()) StopListening();
        if (bits & MAIN_EVENT_ERROR) {
            std::string error;
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(activation_state_mutex_);
                error = last_error_message_;
                generation = last_error_generation_;
            }
            if (generation == voice_generation_.load() && AssistantScreen::IsActive() && !StandbyScreen::IsActive()) {
                SetDeviceState(kDeviceStateIdle);
                Alert(error.c_str(), error.c_str(), "cloud_slash");
            }
        }
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            constexpr unsigned kMaxPackets = 8;
            for (unsigned sent = 0; sent < kMaxPackets; ++sent) {
                auto packet = audio_service_.PopPacketFromSendQueue();
                if (!packet) break;
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) break;
                // 连续收音时也必须交还语音控制队列，避免停止消息长期饥饿。
                if (sent + 1 == kMaxPackets) voice_events_.fetch_or(MAIN_EVENT_SEND_AUDIO);
            }
        }
        if ((bits & MAIN_EVENT_WAKE_WORD_DETECTED) && xiaozhi_voice_active_) {
            OnWakeWordDetected();
        }
        voice_events_queued_.store(false, std::memory_order_release);
        if (voice_events_.load(std::memory_order_acquire) != 0) DispatchVoiceEvents();
    })) {
        voice_events_queued_.store(false, std::memory_order_release);
    }
}

void Application::DispatchClockTick() {
    const int tick = clock_ticks_.fetch_add(1) + 1;
    if (!status_work_queued_.exchange(true)) {
        if (!ScreenLvAsync([](void* context) {
            auto* app = static_cast<Application*>(context);
            static bool sd_error_notified = false;
            if (!sd_error_notified && SdCardManager::GetInstance().HasIoError() &&
                !StandbyScreen::IsActive()) {
                ScreenShowToast("SD 卡读写异常");
                sd_error_notified = true;
            }
            Board::GetInstance().GetDisplay()->UpdateStatusBar();
            app->status_work_queued_.store(false, std::memory_order_release);
        }, this)) status_work_queued_.store(false);
    }
    if (boot_ready_.load(std::memory_order_acquire) && tick % 30 == 0 &&
        !StandbyScreen::IsActive() && !storage_retry_queued_.exchange(true)) {
        if (!Schedule([this]() {
            SsidManager::GetInstance().RetryStorage();
            storage_retry_queued_.store(false, std::memory_order_release);
        }, TaskDomain::Storage)) storage_retry_queued_.store(false);
    }
    const bool kick = activation_kick_.load(std::memory_order_acquire);
    if (boot_ready_.load(std::memory_order_acquire) && !chat_config_ready_.load() &&
        (tick % 10 == 0 || kick) && !StandbyScreen::IsActive() &&
        !activation_paused_.load() && !activation_work_queued_.exchange(true)) {
        if (!Schedule([this]() {
            auto& board = Board::GetInstance();
            if (!StandbyScreen::IsActive() && !activation_paused_.load() &&
                board.IsNetworkReady() && !board.IsWifiConfigMode()) {
                activation_kick_.store(false, std::memory_order_release);
                if (!boot_version_checked_.load()) {
                    Ota ota;
                    CheckNewVersion(ota);
                    if (!activation_paused_.load()) boot_version_checked_.store(true);
                }
                CheckChatActivation();
            }
            activation_work_queued_.store(false, std::memory_order_release);
        }, TaskDomain::Network)) activation_work_queued_.store(false);
    }
}

void Application::MainEventLoop() {
    constexpr EventBits_t voice_mask = MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
                                      MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_ERROR |
                                      MAIN_EVENT_STOP_VOICE | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_PREPARE_VOICE;
    for (;;) {
        const auto bits = xEventGroupWaitBits(event_group_, voice_mask | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_RELEASE_AUDIO,
                                             pdTRUE, pdFALSE, portMAX_DELAY);
        voice_events_.fetch_or(bits & voice_mask, std::memory_order_acq_rel);
        if (voice_events_.load(std::memory_order_acquire) != 0) DispatchVoiceEvents();
        if (bits & MAIN_EVENT_CLOCK_TICK) DispatchClockTick();
        if (audio_release_pending_.load(std::memory_order_acquire) != 0) DispatchAudioReleases();
    }
}

void Application::DispatchAudioReleases() {
    if (audio_release_queued_.exchange(true)) return;
    if (!Schedule([this]() {
        {
            // 与重新申请串行；新会话申请会清除同一调用方尚未执行的旧释放请求。
            std::lock_guard<std::recursive_mutex> lock(audio_service_mutex_);
            const auto released = audio_release_pending_.exchange(0, std::memory_order_acq_rel);
            if (released != 0) ReleaseAudioClients(released);
        }
        audio_release_queued_.store(false, std::memory_order_release);
        if (audio_release_pending_.load() != 0) xEventGroupSetBits(event_group_, MAIN_EVENT_RELEASE_AUDIO);
    }, TaskDomain::Audio)) audio_release_queued_.store(false);
}
void Application::OnWakeWordDetected() {
    if (!xiaozhi_voice_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!Board::GetInstance().EnsureNetworkReady() || !protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected("Hi 钛灵");
        SetListeningMode(chat_cloud::UsesXiaozhiProtocol() ? kListeningModeManualStop :
                     (aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime));
#else
        SetListeningMode(chat_cloud::UsesXiaozhiProtocol() ? kListeningModeManualStop :
                     (aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime));
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this, state]() { SetDeviceState(state); })) ESP_LOGE(TAG, "state change dispatch failed");
        return;
    }
    if (state == kDeviceStateIdle && SettingsTestAgingScreen::IsActive()) {
        state = kDeviceStateAudioTesting;
    }
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    const auto previous_state = device_state_.load(std::memory_order_acquire);
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            ApplyIdleStatusBar();
            display->SetEmotion("neutral");
            // 屏级会话：Idle 回落只停语音处理，不重新打开唤醒词
            if (xiaozhi_voice_active_ && audio_running_) {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            // 百问AI 会话中：TTS stop 重回监听时保留对话，不切回默认芯片表情
            if (!AssistantScreen::IsActive()) {
                display->SetEmotion("neutral");
            }

            // Make sure the audio processor is running
            if (xiaozhi_voice_active_ && audio_running_ && protocol_ &&
                !audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (xiaozhi_voice_active_ && audio_running_) {
                if (listening_mode_ != kListeningModeRealtime) {
                    audio_service_.EnableVoiceProcessing(false);
                }
                // 会话内不启用唤醒词打断；Realtime 模式靠 AEC 全双工
                audio_service_.EnableWakeWordDetection(false);
                audio_service_.ResetDecoder();
            }
            break;
        default:
            // Do nothing
            break;
    }

    // 全屏聆听：仅 Listening 且仍按住才显示；Connecting/Idle/Speaking 同步收起
    if (AssistantScreen::IsActive() &&
        (state == kDeviceStateIdle || state == kDeviceStateConnecting ||
         state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
        AssistantScreen::SyncPttOverlay();
    }

    PowerPolicy::GetInstance().NotifyDeviceState(state);
}

void Application::SetLocalChatTtsEnabled(bool enabled) {
    chat_cloud::SaveLocalTtsEnabled(enabled);
    if (chat_cloud::UsesXiaozhiProtocol() && !enabled && audio_running_) {
        audio_service_.ResetDecoder();
        if (device_state_ == kDeviceStateSpeaking) {
            SetDeviceState(kDeviceStateIdle);
        }
    }
}

void Application::Reboot() {
    if (boot_ready_.load() && !IsVoiceTask()) {
        if (!Schedule([this]() { Reboot(); })) ESP_LOGE(TAG, "reboot dispatch failed");
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    protocol_ready_ = false;
    xiaozhi_voice_active_ = false;
    StopAudioServiceIfRunning();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string current_version = ota.GetCurrentVersion();
    if (current_version.empty()) {
        const esp_app_desc_t* app_desc = esp_app_get_description();
        current_version = (app_desc != nullptr && app_desc->version[0] != '\0') ? app_desc->version : "—";
    }
    std::string new_version = url.empty() ? ota.GetFirmwareVersion() : Lang::Strings::OTA_MANUAL;
    if (new_version.empty()) {
        new_version = "—";
    }
    return UpgradeFirmwareUrl(upgrade_url, current_version, new_version);
}

bool Application::UpgradeFirmwareUrl(const std::string& url, const std::string& current_version,
                                     const std::string& new_version) {
    std::string upgrade_url = url;
    std::string cur = current_version.empty() ? "—" : current_version;
    std::string next = new_version.empty() ? "—" : new_version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }

    SetDeviceState(kDeviceStateUpgrading);

    OtaTouchPause touch_pause;
    // 全屏升级页：锁键 + 版本/进度；须在非 LVGL 线程调用
    if (!OtaUpgradeScreen::Show(cur.c_str(), next.c_str())) {
        ESP_LOGW(TAG, "OTA upgrade UI show incomplete; continuing with keys locked");
    }
    OtaUpgradeScreen::SetProgress(0);

    // 顶栏文案 + 提示音；进度改由全屏页展示（墨水屏 SetChatMessage 在非百问页无效）
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download");
    vTaskDelay(pdMS_TO_TICKS(800));  // 给墨水首帧局刷时间

    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    StopAudioServiceIfRunning();
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!Board::GetInstance().EnsureNetworkReady()) {
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        ESP_LOGE(TAG, "Firmware upgrade aborted: network not ready");
        OtaUpgradeScreen::Dismiss();
        if (xiaozhi_voice_active_) {
            EnsureAudioServiceRunning();
        }
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    Ota ota;
    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [](int progress, size_t /*speed*/) {
        OtaUpgradeScreen::SetProgress(progress);
    });

    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed, continuing operation...");
        OtaUpgradeScreen::Dismiss();
        if (xiaozhi_voice_active_) {
            EnsureAudioServiceRunning();
        }
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    OtaIgnore::Clear();
    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint(Lang::Strings::OTA_SUCCESS_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(1200));  // 让 100% 有机会上屏
    // 成功路径直接重启，无需 Dismiss（复位后自然清空）
    Reboot();
    return true;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!IsVoiceTask()) {
        if (!Schedule([this, wake_word]() { WakeWordInvoke(wake_word); })) {
            ESP_LOGE(TAG, "wake word dispatch failed");
        }
        return;
    }
    if (!xiaozhi_voice_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!Board::GetInstance().EnsureNetworkReady() || !protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(chat_cloud::UsesXiaozhiProtocol() ? kListeningModeManualStop :
                     (aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime));
#else
        SetListeningMode(chat_cloud::UsesXiaozhiProtocol() ? kListeningModeManualStop :
                     (aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime));
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

void Application::SendMcpMessage(const std::string& payload) {
    // protocol_ 可能在排队期间被 TeardownProtocol() 拆掉（离开助手页）。
    // 真正发送时必须再判空，避免 LoadProhibited。
    if (IsVoiceTask()) {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        return;
    }

    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        if (!xiaozhi_voice_active_ || !audio_running_) {
            return;
        }
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

bool Application::IsActivationPollingBusy() {
    std::lock_guard<std::mutex> lock(activation_poll_mutex_);
    return activation_poll_busy_;
}
