#include "pcm_audio_processor.h"

void PcmAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
}

void PcmAudioProcessor::Deinitialize() {
    is_running_ = false;
    codec_ = nullptr;
    frame_samples_ = 0;
}

void PcmAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

    if (codec_->input_channels() == 2) {
        // If input channels is 2, we need to fetch the left channel data
        auto mono_data = std::vector<int16_t>(data.size() / 2);
        for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
            mono_data[i] = data[j];
        }
        output_callback_(std::move(mono_data));
    } else {
        output_callback_(std::move(data));
    }
}

void PcmAudioProcessor::Start() {
    is_running_ = true;
}

void PcmAudioProcessor::Stop() {
    is_running_ = false;
}

bool PcmAudioProcessor::IsRunning() {
    return is_running_;
}

void PcmAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

size_t PcmAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}
