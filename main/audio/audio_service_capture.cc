#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "processors/no_audio_processor.h"
#include <new>

bool AudioService::StartPcmCapture(std::function<bool(const int16_t*, size_t)> callback, std::string* error) {
    std::lock_guard<std::mutex> feed_lock(pcm_feed_mutex_);
    const EventBits_t busy = AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                             AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_PCM_CAPTURE_RUNNING |
                             AS_EVENT_PLAYBACK_NOT_EMPTY;
    if (error) error->clear();
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (service_stopped_) return fail(Lang::Strings::AUDIO_SERVICE_NOT_READY);
    if (!callback) return fail(Lang::Strings::AUDIO_CALLBACK_UNAVAILABLE);
    if (capture_processor_) return fail(Lang::Strings::AUDIO_RECORDING_BUSY);
    if (xEventGroupGetBits(event_group_) & busy)
        return fail(Lang::Strings::AUDIO_MIC_BUSY);
    auto processor = std::unique_ptr<AudioProcessor>(new (std::nothrow) NoAudioProcessor());
    if (!processor) return fail(Lang::Strings::AUDIO_RECORDING_MEMORY);
    processor->Initialize(codec_, OPUS_FRAME_DURATION_MS, nullptr);
    if (processor->GetFeedSize() == 0) return fail(Lang::Strings::AUDIO_PROCESSOR_FAILED);
    processor->OnOutput([this](std::vector<int16_t>&& data) { HandleCapturedAudio(std::move(data)); });
    processor->Start();
    {
        std::lock_guard<std::mutex> lock(pcm_capture_mutex_);
        pcm_capture_callback_ = std::move(callback);
        pcm_capture_generation_.fetch_add(1);
    }
    capture_processor_ = std::move(processor);
    audio_input_need_warmup_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_PCM_CAPTURE_RUNNING);
    return true;
}

void AudioService::StopPcmCapture() {
    {
        std::lock_guard<std::mutex> lock(pcm_capture_mutex_);
        xEventGroupClearBits(event_group_, AS_EVENT_PCM_CAPTURE_RUNNING);
        pcm_capture_generation_.fetch_add(1);
        pcm_capture_callback_ = nullptr;
    }
    // 输入读取可能尚未返回，代号校验阻止它送入下一轮采集。
    std::lock_guard<std::mutex> feed_lock(pcm_feed_mutex_);
    if (capture_processor_) {
        capture_processor_->Stop();
        capture_processor_->Deinitialize();
        capture_processor_.reset();
    }
}

void AudioService::HandleCapturedAudio(std::vector<int16_t>&& data) {
    std::lock_guard<std::mutex> lock(pcm_capture_mutex_);
    if (pcm_capture_callback_ && !pcm_capture_callback_(data.data(), data.size())) {
        xEventGroupClearBits(event_group_, AS_EVENT_PCM_CAPTURE_RUNNING);
        pcm_capture_callback_ = nullptr;
    }
}

void AudioService::FailPcmCapture(uint32_t generation) {
    std::lock_guard<std::mutex> lock(pcm_capture_mutex_);
    if (generation != pcm_capture_generation_.load()) return;
    if (pcm_capture_callback_) pcm_capture_callback_(nullptr, 0);
    pcm_capture_callback_ = nullptr;
    xEventGroupClearBits(event_group_, AS_EVENT_PCM_CAPTURE_RUNNING);
}
