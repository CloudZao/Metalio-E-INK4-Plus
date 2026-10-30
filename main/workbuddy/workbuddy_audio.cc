#include "assets/lang_workbuddy.h"
#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "workbuddy_audio.h"
#include "application.h"
#include "metalio_audio.h"

namespace workbuddy {
bool AudioCapture::Start(std::function<bool(const int16_t*, size_t)> receiver,
                         std::function<bool()> still_needed, std::string& error) {
    error.clear();
    if (acquired_) { error = Lang::Strings::AUDIO_RECORDING_BUSY; return false; }
    if (!receiver || !still_needed || !still_needed()) { error = Lang::Strings::WB_RECORDING_CANCELLED; return false; }
    if (!MetalioAudio_ModeReady()) {
        error = MetalioAudio_GetState() == MetalioAudioState::Failed ?
                Lang::Strings::AUDIO_MODULE_INIT_FAILED : Lang::Strings::AUDIO_MODULE_NOT_READY;
        return false;
    }
    auto& app = Application::GetInstance();
    power_ = std::make_unique<PowerNeedHold>(PowerNeed::LocalMic);
    acquired_ = app.AcquireAudioService(Application::AudioClient::WorkBuddy, still_needed);
    if (acquired_ && still_needed()) {
        capturing_ = app.GetAudioService().StartPcmCapture(std::move(receiver), &error);
    }
    if (!capturing_) {
        if (error.empty()) error = still_needed() ? Lang::Strings::AUDIO_SERVICE_START_FAILED : Lang::Strings::WB_RECORDING_CANCELLED;
        Stop();
    }
    return capturing_;
}

void AudioCapture::Stop() {
    auto& app = Application::GetInstance();
    if (capturing_) {
        app.GetAudioService().StopPcmCapture();
        capturing_ = false;
    }
    if (acquired_) {
        acquired_ = false;
        app.ReleaseAudioService(Application::AudioClient::WorkBuddy);
    }
    power_.reset();
}
}
