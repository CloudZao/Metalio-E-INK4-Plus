#include <algorithm>
#include <cstring>
#include <string>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "boards/common/display/frontlight.h"
#include "business/conversation/conversation_controller.h"
#include "business/conversation/conversation_presentation.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/runtime/input/binding_dispatch.h"
#include "ui/common/runtime/input/boot_key_handler.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/runtime/screen_common_nav.h"

static uint32_t s_held = 0;
static uint32_t s_ptt = 0;
static uint32_t s_long = 0;
static uint32_t s_cancelled = 0;
static int s_unmuted_volume = 50;
static BindingKey s_repeat_key = BindingKey::Count;

static bool Has(BindingMask mask, BindingAction action) {
    return (mask & BindingBit(action)) != 0;
}

static bool IsAiPage() {
    return ConversationPresentation::IsActive() || std::strcmp(VkKey_ActiveScreen(), "WorkBuddyScreen") == 0;
}

static void BeginBootGesture(BindingKey key) {
    const uint32_t bit = uint32_t{1} << static_cast<unsigned>(key);
    if ((s_ptt & bit) != 0) {
        return;
    }
    if (s_ptt == 0) {
        BootKey_OnPressDown();
    }
    s_ptt |= bit;
}

static void Release(BindingKey key) {
    const uint32_t bit = uint32_t{1} << static_cast<unsigned>(key);
    s_held &= ~bit;
    if (s_ptt & bit) {
        s_ptt &= ~bit;
        if (s_ptt == 0) {
            BootKey_OnPressUp();
        }
    }
    if (s_repeat_key == key) {
        VkPageRepeatStop();
        s_repeat_key = BindingKey::Count;
    }
}

static void Execute(BindingKey key, BindingMask mask, bool long_press) {
    for (unsigned action = 1; action < static_cast<unsigned>(BindingAction::Count); ++action) {
        if ((mask & (BindingMask{1} << action)) && !KeyBindingsActionSupported(static_cast<BindingAction>(action))) {
            return;
        }
    }
    if (Has(mask, BindingAction::Record) && IsAiPage()) {
        BeginBootGesture(key);
        BootKey_OnLongPress();
        return;
    }
    if (Has(mask, BindingAction::OpenAi) && !IsAiPage()) {
        const char *screen = VkKey_ActiveScreen();
        if (ScreenIsHome() || std::strncmp(screen, "book", 4) == 0) {
            const uint32_t bit = uint32_t{1} << static_cast<unsigned>(key);
            if (Has(mask, BindingAction::Record) && (s_held & bit)) {
                BeginBootGesture(key);
                BootKey_OnLongPress();
            } else if (auto open = VkKey_GetBootLongPress(screen)) {
                open();
            }
            return;
        }
    }
    if (Has(mask, BindingAction::ReaderSettings) && (std::strcmp(VkKey_ActiveScreen(), "book_read") == 0)) {
        if (auto click = VkKey_GetBootClick(VkKey_ActiveScreen())) {
            click();
        }
        return;
    }
    if (Has(mask, BindingAction::Interrupt) && IsAiPage()) {
        if (ConversationPresentation::IsActive()) {
            ConversationController::GetInstance().AbortSpeakingToIdle();
        }
        return;
    }
    if (Has(mask, BindingAction::Back) && Has(mask, BindingAction::Previous)) {
        if (long_press && VkKey_OnLongPress("vk_prev")) {
            s_repeat_key = key;
            return;
        }
        VkKey_Dispatch("vk_prev");
        return;
    }
    if (Has(mask, BindingAction::Previous) || Has(mask, BindingAction::Next)) {
        if (VkKey_DispatchPage(Has(mask, BindingAction::Previous) ? "vk_prev" : "vk_next", long_press)) {
            if (long_press && VkPageRepeatIsActive()) {
                s_repeat_key = key;
            }
            return;
        }
    }
    if (Has(mask, BindingAction::Back)) {
        VkKey_Dispatch("vk_home");
        return;
    }
    if (Has(mask, BindingAction::Home)) {
        ScreenRequestHome();
        return;
    }
    if (Has(mask, BindingAction::Shutdown)) {
        PowerPolicy::GetInstance().RequestPowerOff();
        return;
    }
    if (Has(mask, BindingAction::Standby)) {
        PowerKey_OnClick();
        return;
    }
    if (Has(mask, BindingAction::FullRefresh)) {
        if (auto *display = LVAdapterDisplay::Instance()) {
            display->RequestFullOnNextCommit();
            lv_obj_invalidate(lv_screen_active());
        }
        return;
    }
    auto &light = Frontlight::GetInstance();
    if (Has(mask, BindingAction::Light)) {
        light.SetEnabled(light.cct() == FrontlightCct::kOff);
        light.SavePreferences();
        return;
    }
    if (Has(mask, BindingAction::LightUp) || Has(mask, BindingAction::LightDown)) {
        light.SetBrightness(
            std::clamp<int>(light.target_brightness() + (Has(mask, BindingAction::LightUp) ? 10 : -10), 0, 100));
        light.SavePreferences();
        return;
    }
    auto *codec = Board::GetInstance().GetAudioCodec();
    const BindingMask volume_actions = BindingBit(BindingAction::VolumeUp) | BindingBit(BindingAction::VolumeDown) |
                                       BindingBit(BindingAction::VolumeMax) | BindingBit(BindingAction::VolumeMin) |
                                       BindingBit(BindingAction::Mute);
    if ((mask & volume_actions) == 0) {
        return;
    }
    if (!codec) {
        ESP_LOGE("BindingDispatch", "operation=volume result=codec_unavailable");
        return;
    }
    int volume = codec->output_volume();
    if (Has(mask, BindingAction::VolumeMax)) {
        volume = 100;
    }
    if (Has(mask, BindingAction::VolumeMin)) {
        volume = 0;
    }
    if (Has(mask, BindingAction::VolumeUp)) {
        volume = std::min(100, volume + 10);
    }
    if (Has(mask, BindingAction::VolumeDown)) {
        volume = std::max(0, volume - 10);
    }
    if (Has(mask, BindingAction::Mute)) {
        if (volume > 0) {
            s_unmuted_volume = volume;
            volume = 0;
        } else {
            volume = s_unmuted_volume;
        }
    }
    codec->SetOutputVolume(volume);
    if (Has(mask, BindingAction::Mute)) {
        return;
    }
    if (auto *display = Board::GetInstance().GetDisplay()) {
        display->ShowNotification(std::string(LangString_VOLUME) + std::to_string(volume));
    }
}

void BindingDispatch(BindingKey key, InputAction action) {
    if (static_cast<unsigned>(key) >= static_cast<unsigned>(BindingKey::Count)) {
        return;
    }
    const uint32_t bit = uint32_t{1} << static_cast<unsigned>(key);
    if (action == InputCancel) {
        s_cancelled |= bit;
        Release(key);
        return;
    }
    if (action == InputPressDown) {
        s_cancelled &= ~bit;
        s_held |= bit;
        s_long &= ~bit;
        if (!UiInputNavigationBlocked()) {
            PowerPolicy::GetInstance().NotifyUserActivity();
        }
        return;
    }
    if (action == InputPressUp) {
        Release(key);
        return;
    }
    if ((s_cancelled & bit) || UiInputNavigationBlocked()) {
        return;
    }
    const bool long_press = action == InputLongPress;
    if (!long_press && action != InputClick && action != InputDoubleClick) {
        return;
    }
    if (!long_press && (s_long & bit)) {
        return;
    }
    if (long_press) {
        s_long |= bit;
    }
    BindingMask mask = KeyBindingsGet(key, long_press);
    const bool reader_volume_key = VkKey_VolumeKeysTurnPages() &&
        ((key == BindingKey::VolumeUp &&
          mask == BindingBit(long_press ? BindingAction::VolumeMax : BindingAction::VolumeUp)) ||
         (key == BindingKey::VolumeDown &&
          mask == BindingBit(long_press ? BindingAction::VolumeMin : BindingAction::VolumeDown)));
    if (reader_volume_key) {
        mask = BindingBit(key == BindingKey::VolumeUp ? BindingAction::Previous : BindingAction::Next);
    }
    if (UiModal::HandleInput(Has(mask, BindingAction::Back) ? UiModalInput::Back : UiModalInput::Other)) {
        return;
    }
    PowerPolicy::GetInstance().NotifyUserActivity();
    // 未配置长按时，只继承短按的翻页功能，不执行短按的全局回退。
    if (long_press && mask == 0) {
        mask = KeyBindingsGet(key, false) & (BindingBit(BindingAction::Previous) | BindingBit(BindingAction::Next));
    }
    Execute(key, mask, long_press);
    if (reader_volume_key && action == InputDoubleClick) {
        Execute(key, mask, long_press);
    }
}

void BindingDispatchCancelAll() {
    s_held = 0;
    s_long = 0;
    s_cancelled = UINT32_MAX;
    s_repeat_key = BindingKey::Count;
    if (s_ptt) {
        BootKey_OnPressUp();
    }
    s_ptt = 0;
    VkPageRepeatStop();
}
