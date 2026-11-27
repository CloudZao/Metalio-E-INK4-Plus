#include <new>
#include <esp_log.h>
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "conversation_controller.h"
#include "assets/lang_config.h"
#include "business/conversation/chat_cloud.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "ConversationQuestion";

bool ConversationController::AskImage() try {
    const auto generation = requests_.Generation();
    return TaskExecutor::Schedule([this, generation]() {
        if (generation != requests_.Generation() || !ConversationPresentation::IsActive() || PowerLifecycleIsStandby() ||
            ChatCloudActiveProvider() != ChatCloudProvider::Baiwen) {
            ConversationPresentation::UploadPhotoQuestion(0, false);
            return;
        }
        const auto turn = ++question_.turn;
        bool ready = false;
        try {
            ready = !RejectUnactivatedListening();
            if (ready && (!SystemState::GetInstance().IsVoiceActive() || !protocol_)) {
                StartSessionOnTask(false, generation);
            }
            ready = ready && SystemState::GetInstance().IsVoiceActive() && protocol_ &&
                    generation == requests_.Generation();
            if (ready && !protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                ready = EnsureNetworkReady([this, generation]() {
                            return generation == requests_.Generation() && ConversationPresentation::IsActive() &&
                                   !PowerLifecycleIsStandby();
                        }) &&
                        protocol_->OpenAudioChannel();
            }
            ready = ready && generation == requests_.Generation();
            if (ready && (question_.stop_waiting || !question_.image_id.empty() ||
                          SystemState::GetInstance().Device() == kDeviceStateSpeaking)) {
                protocol_->SendAbortSpeaking();
            }
            question_.image_id.clear();
            question_.stop_waiting = ready;
            if (generation == requests_.Generation()) {
                SetDeviceState(kDeviceStateIdle);
            }
        } catch (const std::bad_alloc&) {
            question_.stop_waiting = false;
            ESP_LOGE(TAG, "image question preparation failed stage=allocate");
            ready = false;
        }
        if (!ConversationPresentation::UploadPhotoQuestion(turn, ready)) {
            question_.stop_waiting = false;
        }
    });
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "image question preparation failed stage=dispatch_allocate");
    return false;
}

bool ConversationController::CompleteQuestion(uint32_t turn, bool recorded, const std::string& image_id,
                                              std::function<void(ImageQuestionResult)> completed) try {
    return TaskExecutor::Schedule([this, turn, recorded, image_id, completed = std::move(completed)]() {
        auto result = ImageQuestionResult::Replaced;
        if (turn == question_.turn && !ConversationPresentation::IsPttHeld() && SystemState::GetInstance().IsVoiceActive() &&
            ConversationPresentation::IsActive() && !PowerLifecycleIsStandby() &&
            ChatCloudActiveProvider() == ChatCloudProvider::Baiwen) {
            result = ImageQuestionResult::Failed;
            try {
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    if (image_id.empty()) {
                        if (recorded) {
                            protocol_->SendStopListening();
                        }
                        result = recorded ? ImageQuestionResult::Sent : ImageQuestionResult::Failed;
                    } else {
                        question_.image_id = image_id;
                        if (protocol_->SendImageQuestion(image_id, recorded)) {
                            result = ImageQuestionResult::Sent;
                        } else {
                            question_.image_id.clear();
                        }
                    }
                    if (result == ImageQuestionResult::Failed) {
                        protocol_->SendAbortSpeaking();
                    }
                } else {
                    ESP_LOGW(TAG, "question deferred stage=channel_closed");
                }
            } catch (const std::bad_alloc&) {
                question_.image_id.clear();
                ESP_LOGE(TAG, "image question failed stage=allocate");
            }
            question_.stop_waiting = false;
        }
        if (completed) {
            completed(result);
        }
    });
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "image question failed stage=dispatch_allocate");
    return false;
}
