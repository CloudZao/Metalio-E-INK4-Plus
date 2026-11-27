#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "infra/tasks/task_create.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "business/conversation/conversation_controller.h"
#include "conversation_controller.h"
#include "assets/lang_bluetooth.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/board/board.h"
#include "business/conversation/chat_cloud.h"
#include "business/conversation/activation_polling.h"
#include "business/yunxin_binding/yunxin_binding.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_notice.h"

static constexpr const char* TAG = "Conversation";

constexpr int kAssistantNetSliceMs = 200;
constexpr int kAssistantNetBudgetMs = 30000;

// 百问语音任务分片等待网络，离页或待机时结束。
bool ConversationController::EnsureNetworkReady(const std::function<bool()>& still_needed) {
    auto& board = Board::GetInstance();
    if (still_needed && !still_needed()) {
        return false;
    }
    if (!ChatCloudAvailable()) {
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
        if (!ConversationPresentation::IsActive() || PowerLifecycleIsStandby() || board.GetNetworkErrorText(nullptr) ||
            (still_needed && !still_needed())) {
            ESP_LOGI(TAG, "assistant network wait aborted: left screen or standby");
            return false;
        }
        if (board.EnsureNetworkReady(kAssistantNetSliceMs)) {
            // 连上后立刻刷成 RSSI 档，勿等被挡住的 CLOCK_TICK
            ConversationPresentation::RefreshStatusBar(true);
            return !still_needed || still_needed();
        }
        // 每秒更新一次当前业务的联网状态。
        if ((elapsed % 1000) == 0) {
            ConversationPresentation::RefreshStatusBar();
        }
    }
    ESP_LOGW(TAG, "assistant network wait timeout");
    // 页面保网和普通空闲断网由 PowerPolicy 统一决定，不在业务超时后暂停 STA。
    return false;
}

ConversationController& ConversationController::GetInstance() {
    static ConversationController instance;
    return instance;
}

bool ConversationController::Initialize(void (*wake)()) {
    if (wake == nullptr || wake_ != nullptr) {
        ESP_LOGE(TAG, "operation=initialize result=invalid_binding");
        return false;
    }
    wake_ = wake;
    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this] { PostEvents(MAIN_EVENT_SEND_AUDIO); };
    AudioRuntime::GetInstance().Configure(std::move(callbacks));
    return true;
}

void ConversationController::PostEvents(uint32_t bits) {
    voice_events_.fetch_or(bits, std::memory_order_acq_rel);
    if (wake_) {
        wake_();
    }
}

void ConversationController::Poll() {
    if (voice_events_.load(std::memory_order_acquire) != 0) {
        DispatchVoiceEvents();
    }
}

void ConversationController::OnSystemReady() {
    FlushPendingXiaozhiVoice();
}

void ConversationController::RefreshConfiguration() {
    if (activation_.Refresh()) {
        if (SystemState::GetInstance().Device() == kDeviceStateIdle) {
            UiRestoreIdleStatus();
        }
        if (SystemState::GetInstance().IsReady() && ConversationPresentation::IsActive()) {
            StartSession(ConversationPresentation::IsPttHeld());
        }
    }
}

void ConversationController::StartSession(bool enter_listening) {
    const auto phase = SystemState::GetInstance().Phase();
    if (phase == SystemPhase::Updating || phase == SystemPhase::Restarting || phase == SystemPhase::Failed) {
        return;
    }
    requests_.Start(enter_listening);
    PostEvents(MAIN_EVENT_PREPARE_VOICE);
}

void ConversationController::StopSession() {
    requests_.Cancel();
    PostEvents(MAIN_EVENT_STOP_VOICE);
}

void ConversationController::StartListening() {
    PostEvents(MAIN_EVENT_START_LISTENING);
}
void ConversationController::StopListening() {
    PostEvents(MAIN_EVENT_STOP_LISTENING);
}
void ConversationController::AbortSpeakingToIdle() {
    PostEvents(MAIN_EVENT_ABORT_SPEAKING);
}

bool ConversationController::SetLocalTtsEnabled(bool enabled) {
    if (TaskExecutor::IsCurrent(TaskDomain::Voice)) {
        return ApplyLocalTtsEnabled(enabled);
    }
    return TaskExecutor::Schedule([this, enabled] { ApplyLocalTtsEnabled(enabled); });
}

bool ConversationController::CloseForSystem() {
    const bool resume = SystemState::GetInstance().IsVoiceActive() || requests_.HasStart() || protocol_ != nullptr;
    requests_.Cancel();
    voice_events_.store(0, std::memory_order_release);
    StopSessionOnTask();
    if (protocol_) {
        TeardownProtocol();
    }
    return resume;
}

void ConversationController::StartSessionOnTask(bool enter_listening, uint32_t generation) {
    const auto still_needed = [this, generation]() {
        return generation == requests_.Generation() && ConversationPresentation::IsActive() && !PowerLifecycleIsStandby();
    };
    if (!still_needed()) {
        return;
    }
    if (enter_listening && RejectUnactivatedListening()) {
        return;
    }
    PowerNeedHold request(PowerNeed::ForegroundNetwork);
    if (SystemState::GetInstance().IsVoiceActive() && voice_session_generation_ != generation) {
        StopSessionOnTask();
    }
    if (Board::GetInstance().GetNetworkErrorText(nullptr)) {
        auto notify = [](void*) {
            if (!ConversationPresentation::IsActive()) {
                return;
            }
            if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
                ConversationPresentation::Notify(hint);
            }
        };
        if (!ConversationPresentation::Dispatch(notify, true)) {
            ConversationPresentation::Dispatch(notify);
        }
        return;
    }
    if (!ChatCloudAvailable()) {
        ESP_LOGD(TAG, "StartSession skipped: blank cloud endpoints");
        ActivationPolling::GetInstance().SetCode("");
        StopSessionOnTask();
        return;
    }

    // SoftAP 配网无互联网：勿切 Connecting / 空等 EnsureNetworkReady
    if (Board::GetInstance().IsWifiConfigMode()) {
        ESP_LOGW(TAG, "StartSession skipped: wifi config mode");
        // 与 EnterWifiConfigMode 一致：状态「配网模式」，勿套「错误」
        UiShowSystemAlert(LangString_WIFI_CONFIG_MODE, LangString_NEED_WIFI_CFG, "gear");
        return;
    }

    // 开机未完成：可进页，但不建协议；记下意图，Start() 收尾后自动兑现
    if (!SystemState::GetInstance().IsReady()) {
        requests_.Defer(generation, enter_listening);
        ESP_LOGI(TAG, "StartSession deferred until boot ready (listen=%d)", enter_listening ? 1 : 0);

        ConversationPresentation::SetStatus(LangString_PLEASE_WAIT);
        ConversationPresentation::SetDisplayMessage("system", LangString_VOICE_STARTING_NET);
        return;
    }

    if (SystemState::GetInstance().IsVoiceActive() && !enter_listening) {
        // 待机唤醒后通道常已断：补等网 + 预开，与进页一致
        if (!still_needed()) {
            return;
        }
        if (protocol_ && !protocol_->IsAudioChannelOpened()) {
            ESP_LOGI(TAG, "Xiaozhi voice: re-preload channel after standby/idle drop");
            SetDeviceState(kDeviceStateConnecting);
            if (!EnsureNetworkReady(still_needed)) {
                if (!still_needed()) {
                    StopSessionOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice: re-preload network not ready");
                SetDeviceState(kDeviceStateIdle);
                if (!PowerLifecycleIsStandby()) {
                    UiRestoreIdleStatus();
                    UiShowSystemAlert(LangString_SERVER_NOT_CONNECTED, LangString_SERVER_NOT_CONNECTED, "cloud_slash");
                }
                return;
            }
            if (!protocol_->OpenAudioChannel()) {
                if (!still_needed()) {
                    StopSessionOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice: re-preload OpenAudioChannel failed");
                SetDeviceState(kDeviceStateIdle);
                UiRestoreIdleStatus();
                return;
            }
            if (!still_needed()) {
                StopSessionOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
        }
        if (still_needed() && requests_.TakeListen(generation) && ConversationPresentation::IsActive() &&
            ConversationPresentation::IsPttHeld() && protocol_ && protocol_->IsAudioChannelOpened()) {
            StartListening();
        }
        return;
    }

    // LOADED 的 Schedule 可能晚于 UNLOADED：离页后不再拉起会话，避免孤儿协议/音频
    if (!still_needed()) {
        ESP_LOGW(TAG, "StartSession skipped: assistant not active");
        return;
    }

    if (!SystemState::GetInstance().IsVoiceActive()) {
        if (!BluetoothAudio_ModeReady()) {
            // 复位与应答等待放在后台，避免阻塞主事件循环。
            if (audio_mode_wait_pending_.exchange(true)) {
                return;
            }

            struct ModeWaitContext {
                ConversationController* app;  // 会话所有者，生命周期覆盖后台任务
                uint32_t generation;          // 启动请求所属代号
            };

            auto* pending = new (std::nothrow) ModeWaitContext{this, generation};
            if (pending == nullptr) {
                audio_mode_wait_pending_.store(false);
                if (still_needed()) {
                    UiShowSystemAlert(LangString_ERROR, LangString_AUDIO_INIT_FAILED, "circle_xmark");
                }
                return;
            }
            const auto started = FirmwareCreateTask(
                [](void* context) {
                    auto* pending = static_cast<ModeWaitContext*>(context);
                    auto* app = pending->app;
                    const auto generation = pending->generation;
                    delete pending;
                    bool ready = false;
                    const auto still_needed = [app, generation] {
                        return generation == app->requests_.Generation() && ConversationPresentation::IsActive() &&
                               !PowerLifecycleIsStandby();
                    };
                    if (still_needed()) {
                        PowerNeedHold request(PowerNeed::ForegroundNetwork);
                        PowerNeedHold power(PowerNeed::LocalMic);
                        ready = BluetoothAudio_WaitModeReady(kBluetoothAudioInitWaitMs, still_needed);
                    }
                    if (!TaskExecutor::Schedule(
                            [app, ready, generation] { app->OnAudioModeReady(generation, ready); })) {
                        app->audio_mode_wait_pending_.store(false);
                        app->requests_.Defer(generation, ConversationPresentation::IsPttHeld());
                        app->PostEvents(MAIN_EVENT_PREPARE_VOICE);
                    }
                    vTaskDeleteWithCaps(nullptr);
                },
                "audio_ready", 4096, pending, 5, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (started != pdPASS) {
                delete pending;
                audio_mode_wait_pending_.store(false);
                if (still_needed()) {
                    UiShowSystemAlert(LangString_ERROR, LangString_AUDIO_INIT_FAILED, "circle_xmark");
                }
            }
            return;
        }
        ESP_LOGI(TAG, "Starting Xiaozhi voice session (%s)",
                 enter_listening ? "hold-through listen" : "PTT: prepare only");

        if (!ChatCloudAvailable() || !activation_.IsReady() || activation_.NeedsBinding()) {
            EnsureProtocolReady();
            return;
        }
        // 先申请音频 DMA，并保持外部时钟源供电，再启动网络协议。
        PowerNeedHold audio_start_power(PowerNeed::LocalMic);
        if (!AudioRuntime::GetInstance().Acquire(AudioClient::Assistant, still_needed)) {
            return;
        }
        EnsureProtocolReady();
        if (!protocol_) {
            AudioRuntime::GetInstance().Release(AudioClient::Assistant);
            return;
        }

        // Ensure* 可能较慢；期间离页则拆协议并停音频，与 StopSession 一致归还占用
        if (!still_needed()) {
            ESP_LOGW(TAG, "StartSession aborted after Ensure*: left screen");
            TeardownProtocol();
            AudioRuntime::GetInstance().Release(AudioClient::Assistant);
            return;
        }

        if (!protocol_) {
            ESP_LOGW(TAG, "StartSession aborted: no protocol from OTA config");
            AudioRuntime::GetInstance().Release(AudioClient::Assistant);
            return;
        }

        SystemState::GetInstance().SetVoiceActive(true);
        TaskExecutor::SetRetained(TaskDomain::Voice, true);
        voice_session_generation_ = generation;

        // 会话内永不启用唤醒词；非 hold-through 时等 BOOT 按下再开语音处理
        if (AudioRuntime::GetInstance().IsRunning()) {
            AudioRuntime::GetInstance().Service().EnableVoiceCapture(false);
        }
    }

    // 长按进页：开通道后直接 manual 聆听，不落 Idle/待命
    if (enter_listening) {
        if (!protocol_) {
            ESP_LOGW(TAG, "Xiaozhi voice hold-through: no protocol");
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
            return;
        }
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!EnsureNetworkReady(still_needed)) {
                if (!still_needed()) {
                    StopSessionOnTask();
                    return;
                }
                ESP_LOGW(TAG, "Xiaozhi voice hold-through: network not ready");
                SetDeviceState(kDeviceStateIdle);
                if (!PowerLifecycleIsStandby()) {
                    UiRestoreIdleStatus();
                    UiShowSystemAlert(LangString_SERVER_NOT_CONNECTED, LangString_SERVER_NOT_CONNECTED, "cloud_slash");
                }
                return;
            }
            if (!protocol_->OpenAudioChannel()) {
                if (!still_needed()) {
                    StopSessionOnTask();
                    return;
                }
                // SetError → MAIN_EVENT_ERROR 已提示，勿双 UiShowSystemAlert
                ESP_LOGW(TAG, "Xiaozhi voice hold-through: OpenAudioChannel failed");
                SetDeviceState(kDeviceStateIdle);
                UiRestoreIdleStatus();
                return;
            }
        }
        // OpenAudioChannel 阻塞期间可能已松手或离页；Connecting 时 StopListening 会忽略
        if (!still_needed()) {
            ESP_LOGI(TAG, "hold-through aborted: left screen during/after channel open");
            StopSessionOnTask();
            return;
        }
        if (!ConversationPresentation::IsBootHeld()) {
            ESP_LOGI(TAG, "hold-through aborted: BOOT released before listen -> Idle");
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
            ConversationPresentation::SyncPttOverlay();
            return;
        }
        SetListeningMode(kListeningModeManualStop);
        return;
    }

    // 预开 WebSocket，缩短首按延迟；失败不退出会话，按下时 StartListening 会再试
    if (protocol_ && !protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!EnsureNetworkReady(still_needed)) {
            ESP_LOGW(TAG, "Xiaozhi voice: preload network not ready; retry on PTT");
            if (!still_needed()) {
                if (!PowerLifecycleIsStandby()) {
                    StopSessionOnTask();
                } else {
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
            UiShowSystemAlert(LangString_SERVER_NOT_CONNECTED, LangString_SERVER_NOT_CONNECTED, "cloud_slash");
        } else if (!protocol_->OpenAudioChannel()) {
            ESP_LOGW(TAG, "Xiaozhi voice: preload OpenAudioChannel failed; retry on PTT");
            if (!still_needed()) {
                StopSessionOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
        } else {
            if (!still_needed()) {
                StopSessionOnTask();
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            UiRestoreIdleStatus();
        }
    } else {
        if (!still_needed()) {
            StopSessionOnTask();
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        UiRestoreIdleStatus();
    }

    // 预连接期间的长按在通道就绪后兑现，松手或离页不补录音。
    if (still_needed() && requests_.TakeListen(generation) && ConversationPresentation::IsActive() &&
        ConversationPresentation::IsPttHeld() && protocol_ && protocol_->IsAudioChannelOpened()) {
        StartListening();
    }
}

void ConversationController::OnAudioModeReady(uint32_t generation, bool ready) {
    audio_mode_wait_pending_.store(false);
    if (!ConversationPresentation::IsActive() || PowerLifecycleIsStandby()) {
        return;
    }
    if (generation != requests_.Generation()) {
        // 旧等待已经取消，当前页面仍需恢复连接，不能沿用旧失败或录音意图。
        StartSession(false);
    } else if (ready) {
        StartSession(ConversationPresentation::IsPttHeld());
    } else {
        SetDeviceState(kDeviceStateIdle);
        UiShowSystemAlert(LangString_ERROR, LangString_AUDIO_INIT_FAILED, "circle_xmark");
    }
}

void ConversationController::StopSessionOnTask() {
    if (!SystemState::GetInstance().IsVoiceActive() && !protocol_) {
        return;
    }
    ESP_LOGI(TAG, "Stopping Xiaozhi voice session");
    SystemState::GetInstance().SetVoiceActive(false);
    TaskExecutor::SetRetained(TaskDomain::Voice, false);
    ++question_.turn;
    question_.image_id.clear();
    question_.stop_waiting = false;
    // 覆盖音频释放到协议关闭之间的收尾阶段，待机任务不能在此期间进入浅睡。
    PowerNeedHold closing(PowerNeed::ForegroundNetwork);

    // Only the last audio consumer releases tasks and I2S DMA.
    AudioRuntime::GetInstance().Release(AudioClient::Assistant);
    TeardownProtocol();

    // 须走 SetDeviceState，否则 PowerPolicy 仍停在 Connecting → NetActive 不断网
    SetDeviceState(kDeviceStateIdle);
}

bool ConversationController::RejectUnactivatedListening() {
    if (!ConversationPresentation::IsActive() || PowerLifecycleIsStandby() ||
        (!activation_.NeedsBinding() && !ActivationPolling::GetInstance().HasCode() &&
         !YunxinBinding::NeedsBinding())) {
        return false;
    }
    if (YunxinBinding::NeedsBinding()) {
        return true;
    }
    auto notify = [](void*) {
        auto& app = ConversationController::GetInstance();
        if (ConversationPresentation::IsActive() && !PowerLifecycleIsStandby() &&
            (app.activation_.NeedsBinding() || ActivationPolling::GetInstance().HasCode())) {
            ConversationPresentation::Notify(ChatCloudIsXiaozhi() ? LangString_XIAOZHI_ACTIVATE_FIRST
                                                 : LangString_DEVICE_ACTIVATE_FIRST);
        }
    };
    if (!ConversationPresentation::Dispatch(notify, true)) {
        ConversationPresentation::Dispatch(notify);
    }
    return true;
}

void ConversationController::FlushPendingXiaozhiVoice() {
    PostEvents(MAIN_EVENT_PREPARE_VOICE);
}

bool ConversationController::EnterContinuousListening() {
    if (!SystemState::GetInstance().IsVoiceActive() || !protocol_) {
        ESP_LOGW(TAG, "EnterContinuousListening ignored: session or protocol not ready");
        return false;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!EnsureNetworkReady()) {
            ESP_LOGE(TAG, "EnterContinuousListening: network not ready");
            SetDeviceState(kDeviceStateIdle);
            if (!PowerLifecycleIsStandby()) {
                UiShowSystemAlert(LangString_SERVER_NOT_CONNECTED, LangString_SERVER_NOT_CONNECTED, "cloud_slash");
            }
            return false;
        }
        if (!protocol_->OpenAudioChannel()) {
            // OpenAudioChannel 内 SetError → MAIN_EVENT_ERROR，勿再 UiShowSystemAlert
            ESP_LOGE(TAG, "EnterContinuousListening: OpenAudioChannel failed");
            SetDeviceState(kDeviceStateIdle);
            return false;
        }
    }

    SetListeningMode(ChatCloudUsesXiaozhiProtocol() ? kListeningModeManualStop : kListeningModeAutoStop);
    return true;
}

void ConversationController::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

bool ConversationController::ApplyLocalTtsEnabled(bool enabled) {
    if (!ChatCloudSaveLocalTtsEnabled(enabled)) {
        return false;
    }
    if (ChatCloudUsesXiaozhiProtocol() && !enabled && AudioRuntime::GetInstance().IsRunning()) {
        AudioRuntime::GetInstance().Service().ResetDecoder();
        if (SystemState::GetInstance().Device() == kDeviceStateSpeaking) {
            SetDeviceState(kDeviceStateIdle);
        }
    }
    return true;
}

void ConversationController::StartListeningOnTask() {
    if (RejectUnactivatedListening()) {
        return;
    }
    const auto generation = requests_.Generation();
    const auto still_needed = [this, generation]() {
        return generation == requests_.Generation() && ConversationPresentation::IsActive() && !PowerLifecycleIsStandby();
    };
    if (!still_needed()) {
        return;
    }
    if (SystemState::GetInstance().Device() == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (SystemState::GetInstance().Device() == kDeviceStateWifiConfiguring) {
        // 原工程配网态长按 BOOT 会进本地录音环回；产品不要此行为
        ESP_LOGI(TAG, "StartListening ignored in wifi configuring (no audio testing)");
        return;
    }

    if (!SystemState::GetInstance().IsReady()) {
        requests_.Defer(generation, true);
        ESP_LOGI(TAG, "StartListening deferred until boot ready");
        ConversationPresentation::SetStatus(LangString_PLEASE_WAIT);
        return;
    }

    if (!SystemState::GetInstance().IsVoiceActive() || !protocol_) {
        if (!activation_.IsChecking() && ConversationPresentation::IsActive()) {
            TaskExecutor::Schedule([this, generation, still_needed]() {
                if (!still_needed()) {
                    return;
                }
                StartSessionOnTask(false, generation);
                if (still_needed() && SystemState::GetInstance().IsVoiceActive() && protocol_ &&
                    ConversationPresentation::IsPttHeld()) {
                    StartListening();
                }
            });
        }
        return;
    }

    if (SystemState::GetInstance().Device() == kDeviceStateConnecting && ConversationPresentation::IsActive()) {
        requests_.Listen(generation);
        return;
    }

    if (SystemState::GetInstance().Device() == kDeviceStateIdle) {
        TaskExecutor::Schedule([this, still_needed]() {
            if (!still_needed() || !SystemState::GetInstance().IsVoiceActive() || !protocol_) {
                return;
            }
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                ESP_LOGI(TAG, "StartListening: audio channel not open, EnsureNetwork+Open");
                if (!EnsureNetworkReady(still_needed)) {
                    if (!still_needed()) {
                        StopSessionOnTask();
                        return;
                    }
                    ESP_LOGE(TAG, "StartListening: network not ready");
                    SetDeviceState(kDeviceStateIdle);
                    if (!PowerLifecycleIsStandby()) {
                        UiRestoreIdleStatus();
                        UiShowSystemAlert(LangString_SERVER_NOT_CONNECTED, LangString_SERVER_NOT_CONNECTED,
                                          "cloud_slash");
                    }
                    return;
                }
                if (!protocol_->OpenAudioChannel()) {
                    if (!still_needed()) {
                        StopSessionOnTask();
                        return;
                    }
                    ESP_LOGE(TAG, "StartListening: OpenAudioChannel failed");
                    SetDeviceState(kDeviceStateIdle);
                    UiRestoreIdleStatus();
                    return;
                }
            } else {
                ESP_LOGI(TAG, "StartListening: reuse open audio channel");
            }

            if (!still_needed()) {
                StopSessionOnTask();
                return;
            }

            // 百问页长按臂听：开通道期间已松手则回待命（BOOT ∪ 屏触 PTT）
            if (ConversationPresentation::IsActive() && !ConversationPresentation::IsPttHeld()) {
                ESP_LOGI(TAG, "StartListening aborted: PTT released before listen");
                SetDeviceState(kDeviceStateIdle);
                UiRestoreIdleStatus();
                ConversationPresentation::SyncPttOverlay();
                return;
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (SystemState::GetInstance().Device() == kDeviceStateSpeaking) {
        TaskExecutor::Schedule([this, still_needed]() {
            if (!still_needed() || !SystemState::GetInstance().IsVoiceActive()) {
                return;
            }
            if (SystemState::GetInstance().Device() != kDeviceStateSpeaking) {
                return;
            }
            AbortSpeaking();
            // 长按打断并开听；若已松手则只落到待命，不进聆听中
            if (ConversationPresentation::IsActive() && !ConversationPresentation::IsPttHeld()) {
                ESP_LOGI(TAG, "StartListening from speaking aborted: PTT released -> Idle");
                if (AudioRuntime::GetInstance().IsRunning()) {
                    AudioRuntime::GetInstance().Service().ResetDecoder();
                }
                SetDeviceState(kDeviceStateIdle);
                ConversationPresentation::SyncPttOverlay();
                return;
            }
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void ConversationController::StopListeningOnTask() {
    requests_.CancelListen();
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), SystemState::GetInstance().Device()) ==
        valid_states.end()) {
        return;
    }

    if (SystemState::GetInstance().Device() == kDeviceStateListening && protocol_) {
        if (ChatCloudActiveProvider() == ChatCloudProvider::Baiwen) {
            SetDeviceState(kDeviceStateIdle);
            question_.stop_waiting = true;
            if (!ConversationPresentation::EndPhotoListening(question_.turn)) {
                question_.stop_waiting = false;
                protocol_->SendAbortSpeaking();
                ESP_LOGE(TAG, "photo listening stop dispatch failed");
            }
        } else {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    }
}

void ConversationController::AbortSpeakingOnTask() {
    if (!SystemState::GetInstance().IsVoiceActive() || SystemState::GetInstance().Device() != kDeviceStateSpeaking) {
        return;
    }
    if (protocol_) {
        protocol_->SendAbortSpeaking();
    }
    if (AudioRuntime::GetInstance().IsRunning()) {
        AudioRuntime::GetInstance().Service().ResetDecoder();
    }
    SetDeviceState(kDeviceStateIdle);
}

void ConversationController::DispatchVoiceEvents() {
    bool expected = false;
    if (!voice_events_queued_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!TaskExecutor::Schedule([this]() {
            struct Completion {
                ConversationController& owner;
                ~Completion() {
                    owner.voice_events_queued_.store(false, std::memory_order_release);
                    if (owner.voice_events_.load(std::memory_order_acquire) != 0) {
                        owner.DispatchVoiceEvents();
                    }
                }
            } completion{*this};
            try {
                const auto bits = voice_events_.exchange(0, std::memory_order_acq_rel);
                // 满队列中的停止意图仍需交付；重进页面或再次按住后不执行旧停止意图。
                if ((bits & MAIN_EVENT_STOP_VOICE) && voice_session_generation_ != requests_.Generation()) {
                    StopSessionOnTask();
                }
                if ((bits & MAIN_EVENT_PREPARE_VOICE) && ConversationPresentation::IsActive() && !PowerLifecycleIsStandby()) {
                    const auto request = requests_.TakeStart();
                    if (request.pending) {
                        StartSessionOnTask(request.listen, request.generation);
                    }
                }
                if ((bits & MAIN_EVENT_STOP_LISTENING) && !ConversationPresentation::IsPttHeld()) {
                    StopListeningOnTask();
                }
                if (bits & MAIN_EVENT_START_LISTENING) {
                    StartListeningOnTask();
                }
                if (bits & MAIN_EVENT_ABORT_SPEAKING) {
                    AbortSpeakingOnTask();
                }
                if (bits & MAIN_EVENT_ERROR) {
                    std::string error;
                    uint32_t generation;
                    {
                        std::lock_guard<std::mutex> lock(error_mutex_);
                        error = last_error_message_;
                        generation = last_error_generation_;
                    }
                    if (generation == requests_.Generation() && ConversationPresentation::IsActive() &&
                        !PowerLifecycleIsStandby()) {
                        SetDeviceState(kDeviceStateIdle);
                        UiShowSystemAlert(error.c_str(), error.c_str(), "cloud_slash");
                    }
                }
                if (bits & MAIN_EVENT_SEND_AUDIO) {
                    constexpr unsigned kMaxPackets = 8;
                    for (unsigned sent = 0; sent < kMaxPackets; ++sent) {
                        auto packet = AudioRuntime::GetInstance().Service().PopPacketFromSendQueue();
                        if (!packet) {
                            break;
                        }
                        if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                            break;
                        }
                        // 连续收音时也必须交还语音控制队列，避免停止消息长期饥饿。
                        if (sent + 1 == kMaxPackets) {
                            voice_events_.fetch_or(MAIN_EVENT_SEND_AUDIO);
                        }
                    }
                }

            } catch (const std::bad_alloc&) {
                ESP_LOGE(TAG, "operation=dispatch result=no_memory; session closing");
                StopSession();
            }
        })) {
        voice_events_queued_.store(false, std::memory_order_release);
    }
}

void ConversationController::SetDeviceState(DeviceState state) {
    auto& system = SystemState::GetInstance();
    if (system.Phase() == SystemPhase::Updating || system.Phase() == SystemPhase::Restarting ||
        system.Phase() == SystemPhase::Failed || system.Device() == state) {
        return;
    }
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            if (system.IsVoiceActive() && AudioRuntime::GetInstance().IsRunning()) {
                AudioRuntime::GetInstance().Service().EnableVoiceCapture(false);
            }
            break;
        case kDeviceStateListening:
            if (system.IsVoiceActive() && AudioRuntime::GetInstance().IsRunning() && protocol_ &&
                !AudioRuntime::GetInstance().Service().IsVoiceCaptureRunning()) {
                if (ChatCloudActiveProvider() == ChatCloudProvider::Baiwen) {
                    if (question_.stop_waiting || !question_.image_id.empty()) {
                        protocol_->SendAbortSpeaking();
                    }
                    question_.image_id.clear();
                    question_.stop_waiting = false;
                    ++question_.turn;
                    AudioRuntime::GetInstance().Service().FlushQueues();
                    if (!ConversationPresentation::BeginPhotoListening(question_.turn)) {
                        SetDeviceState(kDeviceStateIdle);
                        ESP_LOGE(TAG, "operation=listen stage=photo result=dispatch_failed");
                        return;
                    }
                }
                protocol_->SendStartListening(listening_mode_);
                AudioRuntime::GetInstance().Service().EnableVoiceCapture(true);
            }
            break;
        case kDeviceStateSpeaking:
            if (system.IsVoiceActive() && AudioRuntime::GetInstance().IsRunning()) {
                AudioRuntime::GetInstance().Service().EnableVoiceCapture(false);
                AudioRuntime::GetInstance().Service().ResetDecoder();
            }
            break;
        default:
            break;
    }
    if (system.PublishDevice(state)) {
        UiPresentConversationState(state);
    }
}

void ConversationController::AbortSpeaking() {
    ESP_LOGI(TAG, "Abort speaking");
    if (protocol_) {
        protocol_->SendAbortSpeaking();
    }
}
