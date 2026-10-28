#include "workbuddy_audio.h"
#include "application.h"
#include "metalio_audio.h"

namespace workbuddy {
bool AudioCapture::Start(std::function<bool(const int16_t*, size_t)> receiver,
                         std::function<bool()> still_needed, std::string& error) {
    error.clear();
    if (acquired_) { error = "上一轮录音尚未结束，请稍后重试"; return false; }
    if (!receiver || !still_needed || !still_needed()) { error = "录音已取消"; return false; }
    if (!MetalioAudio_ModeReady()) {
        error = MetalioAudio_GetState() == MetalioAudioState::Failed ?
                "音频模块初始化失败，请检查模块连接后重启设备" : "音频模块尚未就绪，请稍后重试";
        return false;
    }
    auto& app = Application::GetInstance();
    power_ = std::make_unique<PowerNeedHold>(PowerNeed::LocalMic);
    acquired_ = app.AcquireAudioService(Application::AudioClient::WorkBuddy, still_needed);
    if (acquired_ && still_needed()) {
        capturing_ = app.GetAudioService().StartPcmCapture(std::move(receiver), &error);
    }
    if (!capturing_) {
        if (error.empty()) error = still_needed() ? "音频服务启动失败，请稍后重试" : "录音已取消";
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
