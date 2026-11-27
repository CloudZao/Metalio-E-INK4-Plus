#include "sysmgmt/audio/audio_runtime.h"
#include "workbuddy_audio.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "assets/lang_workbuddy.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"

bool WorkBuddyAudioCapture::Start(std::function<bool(const uint8_t*, size_t, size_t)> receiver,
                                  std::function<bool()> still_needed, std::string& error) {
    error.clear();
    if (acquired_) {
        error = LangString_AUDIO_RECORDING_BUSY;
        return false;
    }
    if (!receiver || !still_needed || !still_needed()) {
        error = LangString_WB_RECORDING_CANCELLED;
        return false;
    }
    if (!BluetoothAudio_ModeReady()) {
        error = BluetoothAudio_GetState() == BluetoothAudioState::Failed ? LangString_AUDIO_MODULE_INIT_FAILED
                                                                   : LangString_AUDIO_MODULE_NOT_READY;
        return false;
    }
    power_ = std::make_unique<PowerNeedHold>(PowerNeed::LocalMic);
    acquired_ = AudioRuntime::GetInstance().Acquire(AudioClient::WorkBuddy, still_needed);
    if (acquired_ && still_needed()) {
        capturing_ = AudioRuntime::GetInstance().Service().StartOpusCapture(std::move(receiver), &error);
    }
    if (!capturing_) {
        if (error.empty()) {
            error = still_needed() ? LangString_AUDIO_SERVICE_START_FAILED : LangString_WB_RECORDING_CANCELLED;
        }
        Stop();
    }
    return capturing_;
}

void WorkBuddyAudioCapture::Stop() {
    if (capturing_) {
        AudioRuntime::GetInstance().Service().StopOpusCapture();
        capturing_ = false;
    }
    if (acquired_) {
        acquired_ = false;
        AudioRuntime::GetInstance().Release(AudioClient::WorkBuddy);
    }
    power_.reset();
}
