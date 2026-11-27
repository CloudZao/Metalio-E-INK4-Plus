#include <new>
#include <esp_log.h>
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "conversation_controller.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "ConversationText";

void ConversationController::ApplyYunxinText(const YunxinTextInbox::Snapshot& snapshot) {
    if (!yunxin_text_inbox_.IsCurrent(snapshot)) {
        return;
    }
    try {
        if (!snapshot.text.empty()) {
            ConversationPresentation::AddYunxinSpeech(snapshot.text.c_str());
        }
        if (snapshot.finish) {
            ConversationPresentation::FinishYunxinSpeech();
        } else {
            ConversationPresentation::FlushYunxinSpeech();
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "NetEase text skipped: PSRAM exhausted");
    }
    yunxin_text_inbox_.Applied(snapshot);
    if (snapshot.after) {
        snapshot.after();
    }
}

void ConversationController::ReceiveYunxinText(const char* text) try {
    yunxin_text_inbox_.Push(text, [this](YunxinTextInbox::Snapshot snapshot) {
        return TaskExecutor::Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
    });
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "NetEase text inbox full: PSRAM exhausted");
}

void ConversationController::QueueYunxinTextUpdate() try {
    yunxin_text_inbox_.Update([this](YunxinTextInbox::Snapshot snapshot) {
        return TaskExecutor::Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
    });
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "NetEase text update deferred: PSRAM exhausted");
}

void ConversationController::QueueYunxinTextBoundary(std::function<void()> after) try {
    yunxin_text_inbox_.Finish(
        [this](YunxinTextInbox::Snapshot snapshot) {
            return TaskExecutor::Schedule([this, snapshot = std::move(snapshot)]() { ApplyYunxinText(snapshot); });
        },
        after);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "NetEase text boundary dropped: PSRAM exhausted");
    TaskExecutor::Schedule(std::move(after));
}
