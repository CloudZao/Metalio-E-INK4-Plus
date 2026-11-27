#include <cstdio>
#include <ctime>
#include "business/conversation/conversation_presentation.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "business/conversation/activation_polling.h"
#include "sysmgmt/system_state.h"
#include "ui/common/drawing/display.h"
#include "ui/common/widgets/binding_panel.h"
#include "ui/apps/assistant/assistant_screen.h"
#include "sysmgmt/power/standby_presentation.h"
#include "sysmgmt/system_notice.h"

static void RestoreActivationStatus() {
    const auto code = ActivationPolling::GetInstance().Code();
    if (code.empty()) {
        return;
    }
    char status[64];
    std::snprintf(status, sizeof(status), LangString_ACTIVATION_CODE_FMT, code.c_str());
    Board::GetInstance().GetDisplay()->SetStatus(status);
}

void UiShowSystemAlert(const char* status, const char* message, const char* emotion) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
}

void UiRestoreIdleStatus() {
    auto display = Board::GetInstance().GetDisplay();
    if (ActivationPolling::GetInstance().HasCode()) {
        RestoreActivationStatus();
        return;
    }
    // 固定文案页：不刷时钟。百问页已声明 kClock，会话 Idle 也走时钟，勿显示「待命」
    if (!display->AllowsIdleStatusClock()) {
        const char* text = display->GetIdleStatusFixedText();
        display->SetStatus((text != nullptr && text[0] != '\0') ? text : LangString_STANDBY);
        return;
    }

    // 首页 / 百问 Idle：直接显示时钟
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= (2025 - 1900)) {
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
        display->SetStatus(time_str);
    } else {
        display->SetStatus("--:--");
    }
}

void UiDismissSystemAlert() {
    if (SystemState::GetInstance().Device() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        UiRestoreIdleStatus();
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void UiPresentConversationState(DeviceState state) {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            UiRestoreIdleStatus();
            display->SetEmotion("neutral");
            break;
        case kDeviceStateConnecting:
            display->SetStatus(LangString_CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(LangString_LISTENING);
            if (!ConversationPresentation::IsActive()) {
                display->SetEmotion("neutral");
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(LangString_SPEAKING);
            break;
        default:
            break;
    }
    if (ConversationPresentation::IsActive()) {
        ConversationPresentation::SyncPttOverlay();
    }
}

void UiRestoreProvisioningStatus() {
    const auto state = SystemState::GetInstance().Device();
    if (state != kDeviceStateWifiConfiguring && state != kDeviceStateActivating) {
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    if (state == kDeviceStateWifiConfiguring) {
        display->SetStatus(LangString_WIFI_CONFIG_MODE);
        display->SetEmotion("gear");
        display->SetChatMessage("system", LangString_NEED_WIFI_CFG);
        return;
    }
    auto& polling = ActivationPolling::GetInstance();
    const auto code = polling.Code();
    char status[64];
    if (code.empty()) {
        display->SetStatus(LangString_CHECKING_NEW_VERSION);
    } else {
        std::snprintf(status, sizeof(status), LangString_ACTIVATION_CODE_FMT, code.c_str());
        display->SetStatus(status);
    }
    display->SetEmotion("neutral");
    if (!polling.IsPaused()) {
        polling.RequestCheck();
    }
}
