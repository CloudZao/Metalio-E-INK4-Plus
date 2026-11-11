#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "processors/no_audio_processor.h"
#include <new>
#include <algorithm>

bool AudioService::StartOpusCapture(std::function<bool(const uint8_t*, size_t, size_t)> callback, std::string* error) {
    std::lock_guard<std::mutex> feed_lock(capture_feed_mutex_);
    const EventBits_t busy = AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                             AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_OPUS_CAPTURE_RUNNING |
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
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!audio_encode_queue_.empty() || !audio_send_queue_.empty() || capture_encoding_)
            return fail(Lang::Strings::AUDIO_MIC_BUSY);
    }
    auto processor = std::unique_ptr<AudioProcessor>(new (std::nothrow) NoAudioProcessor());
    if (!processor) return fail(Lang::Strings::AUDIO_RECORDING_MEMORY);
    processor->Initialize(codec_, OPUS_FRAME_DURATION_MS, nullptr);
    if (processor->GetFeedSize() == 0) return fail(Lang::Strings::AUDIO_PROCESSOR_FAILED);
    processor->OnOutput([this](std::vector<int16_t>&& data) { HandleCapturedAudio(std::move(data)); });
    processor->Start();
    {
        std::lock_guard<std::mutex> lock(opus_capture_mutex_);
        opus_capture_callback_ = std::move(callback);
        opus_capture_generation_.fetch_add(1);
    }
    capture_processor_ = std::move(processor);
    audio_input_need_warmup_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_OPUS_CAPTURE_RUNNING);
    return true;
}

void AudioService::StopOpusCapture() {
    xEventGroupClearBits(event_group_, AS_EVENT_OPUS_CAPTURE_RUNNING);
    audio_queue_cv_.notify_all();
    // 先封闭输入，再排空编码队列；停止期间仍交付已采集的尾帧。
    std::lock_guard<std::mutex> feed_lock(capture_feed_mutex_);
    if (capture_processor_) {
        capture_processor_->Stop();
        capture_processor_->Deinitialize();
        capture_processor_.reset();
    }
    {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return !capture_encoding_ && std::none_of(audio_encode_queue_.begin(), audio_encode_queue_.end(),
                [](const auto& task) { return task->type == kAudioTaskTypeEncodeToCapture; });
        });
    }
    std::lock_guard<std::mutex> lock(opus_capture_mutex_);
    opus_capture_generation_.fetch_add(1);
    opus_capture_callback_ = nullptr;
}

void AudioService::HandleCapturedAudio(std::vector<int16_t>&& data) {
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypeEncodeToCapture;
    task->capture_generation = opus_capture_generation_.load();
    task->pcm = std::move(data);
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
        return service_stopped_ || audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;
    });
    if (service_stopped_) return;
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

void AudioService::HandleCapturedOpus(const std::vector<uint8_t>& packet, size_t samples, uint32_t generation) {
    std::lock_guard<std::mutex> lock(opus_capture_mutex_);
    if (generation != opus_capture_generation_.load()) return;
    if (opus_capture_callback_ && !opus_capture_callback_(packet.data(), packet.size(), samples)) {
        xEventGroupClearBits(event_group_, AS_EVENT_OPUS_CAPTURE_RUNNING);
        opus_capture_callback_ = nullptr;
    }
}

void AudioService::FailOpusCapture(uint32_t generation) {
    std::lock_guard<std::mutex> lock(opus_capture_mutex_);
    if (generation != opus_capture_generation_.load()) return;
    if (opus_capture_callback_) opus_capture_callback_(nullptr, 0, 0);
    opus_capture_callback_ = nullptr;
    xEventGroupClearBits(event_group_, AS_EVENT_OPUS_CAPTURE_RUNNING);
}
