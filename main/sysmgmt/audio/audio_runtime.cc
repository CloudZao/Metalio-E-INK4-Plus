#include <utility>
#include <esp_log.h>
#include "audio_runtime.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/board/board.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "AudioRuntime";
static constexpr uint32_t kNetworkClients =
    static_cast<uint32_t>(AudioClient::Assistant) | static_cast<uint32_t>(AudioClient::WorkBuddy);

AudioRuntime& AudioRuntime::GetInstance() {
    static AudioRuntime instance;
    return instance;
}

void AudioRuntime::Configure(AudioServiceCallbacks callbacks) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    service_.SetCallbacks(callbacks);
}

bool AudioRuntime::Acquire(AudioClient client, std::function<bool()> still_needed) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (still_needed && !still_needed()) {
        return false;
    }
    const auto bit = static_cast<uint32_t>(client);
    release_pending_.fetch_and(~bit, std::memory_order_acq_rel);
    clients_ |= bit;
    EnsureStarted();
    if (!running_ || (still_needed && !still_needed())) {
        ReleaseClients(bit);
        return false;
    }
    return true;
}

void AudioRuntime::Release(AudioClient client) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ReleaseClients(static_cast<uint32_t>(client));
}

void AudioRuntime::ReleaseClients(uint32_t clients) {
    // 调用方持锁；保留联网标记，直到采集与编解码任务真正退出。
    const auto previous = clients_.load();
    const auto remaining = previous & ~clients;
    clients_.store(remaining | (previous & clients & kNetworkClients));
    if (remaining == 0) {
        StopIfRunning();
    }
    clients_.store(remaining, std::memory_order_release);
}

void AudioRuntime::RequestRelease(AudioClient client) {
    release_pending_.fetch_or(static_cast<uint32_t>(client), std::memory_order_release);
    RetryPendingReleases();
}

void AudioRuntime::RetryPendingReleases() {
    if (release_pending_.load(std::memory_order_acquire) == 0 || release_queued_.exchange(true)) {
        return;
    }
    if (!TaskExecutor::Schedule(
            [this]() {
                {
                    // 重新申请会在同一把锁内清除该调用方的旧释放请求。
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    const auto released = release_pending_.exchange(0, std::memory_order_acq_rel);
                    if (released != 0) {
                        ReleaseClients(released);
                    }
                }
                release_queued_.store(false, std::memory_order_release);
                RetryPendingReleases();
            },
            TaskDomain::Audio)) {
        release_queued_.store(false, std::memory_order_release);
    }
}

void AudioRuntime::DropLocalPlayback(bool release_hardware) {
    if (!local_playback_owner_.exchange(nullptr)) {
        return;
    }
    clients_.fetch_and(~static_cast<uint32_t>(AudioClient::AppPlayback), std::memory_order_acq_rel);
    if (release_hardware) {
        if (auto* codec = Board::GetInstance().GetAudioCodec()) {
            codec->ReleaseHardware();
        }
    }
}

bool AudioRuntime::AcquireLocalPlayback(void* owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!owner || clients_.load() || running_.load() || SystemState::GetInstance().Device() != kDeviceStateIdle ||
        !BluetoothAudio_ModeReady()) {
        return false;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || codec->output_channels() != 1) {
        return false;
    }
    try {
        codec->Start();
    } catch (...) {
        codec->ReleaseHardware();
        throw;
    }
    if (!codec->HardwareReady()) {
        return false;
    }
    clients_.fetch_or(static_cast<uint32_t>(AudioClient::AppPlayback), std::memory_order_acq_rel);
    local_playback_owner_.store(owner);
    return true;
}

bool AudioRuntime::AcquireLocalRecording(void* owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || codec->input_sample_rate() != 16000 ||
        (codec->input_channels() != 1 && !(codec->input_channels() == 2 && codec->input_reference()))) {
        return false;
    }
    return AcquireLocalPlayback(owner);
}

bool AudioRuntime::ReadLocalRecording(void* owner, std::vector<int16_t>& pcm) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!owner || local_playback_owner_.load() != owner || !BluetoothAudio_ModeReady()) {
        return false;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->HardwareReady()) {
        return false;
    }
    const size_t channels = codec->input_channels();
    pcm.resize(320 * channels);
    if (!codec->InputData(pcm)) {
        return false;
    }
    if (channels == 2) {
        for (size_t i = 0; i < 320; ++i) {
            pcm[i] = pcm[i * 2];
        }
        pcm.resize(320);
    }
    return true;
}

bool AudioRuntime::WriteLocalPlayback(void* owner, std::vector<int16_t>& pcm) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!owner || local_playback_owner_.load() != owner || !BluetoothAudio_ModeReady()) {
        return false;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec && codec->HardwareReady() && codec->OutputData(pcm);
}

void AudioRuntime::ReleaseLocalPlayback(void* owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!owner || local_playback_owner_.load() != owner) {
        return;
    }
    DropLocalPlayback(true);
}

void AudioRuntime::EnsureStarted() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DropLocalPlayback(false);
    if (!BluetoothAudio_ModeReady()) {
        BluetoothAudio_RequestModeInit();
        return;
    }
    if (!initialized_) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            ESP_LOGE(TAG, "operation=start stage=codec result=unavailable");
            return;
        }
        if (!service_.Initialize(codec)) {
            return;
        }
        initialized_ = true;
    }
    if (!running_) {
        service_.Start();
        if (!service_.IsStarted()) {
            ESP_LOGE(TAG, "operation=start stage=service result=failed");
            return;
        }
        running_.store(true, std::memory_order_release);
        TaskExecutor::SetRetained(TaskDomain::Audio, true);
        ESP_LOGI(TAG, "service started");
    }
}

void AudioRuntime::StopIfRunning(bool force) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DropLocalPlayback(true);
    // 重启前必须退出音频任务并停止 I2S DMA，不能被老化占用保护跳过。
    if (!force && (clients_ & static_cast<uint32_t>(AudioClient::AgingTest))) {
        ESP_LOGW(TAG, "operation=stop result=deferred reason=aging_test");
        return;
    }
    if (!running_) {
        if (force) {
            service_.Stop();
        }
        return;
    }
    service_.EnableVoiceCapture(false);
    service_.Stop();
    running_.store(false, std::memory_order_release);
    TaskExecutor::SetRetained(TaskDomain::Audio, false);
    ESP_LOGI(TAG, "service stopped");
}

void AudioRuntime::StopIfUnused() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (clients_.load() == 0) {
        StopIfRunning();
    }
}

bool AudioRuntime::HasNetworkWork() const {
    return (clients_.load(std::memory_order_acquire) & kNetworkClients) != 0;
}

bool AudioRuntime::HasWork() const {
    return clients_.load(std::memory_order_acquire) != 0 || running_.load(std::memory_order_acquire) ||
           release_pending_.load(std::memory_order_acquire) != 0 || release_queued_.load(std::memory_order_acquire);
}
