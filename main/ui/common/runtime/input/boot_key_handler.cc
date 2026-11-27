#include "business/diagnostics/log_upload.h"

/**
 * @brief BOOT/电源键分发
 */
#include <atomic>
#include <esp_log.h>
#include "ui/apps/cloud/cloud_screen.h"
#include "sysmgmt/update/update_presentation.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/input/boot_key_handler.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "boards/common/storage/usb_virtual_disk.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/runtime/modal.h"

#define TAG "BootKey"

enum class BootKeyDispatchPhase {
    Action, // 点击、双击或长按
    PressDown, // 记录按下是否交给业务
    PressUp // 已交付按下的收尾，不被后来打开的窗口截断
};

static std::atomic<bool> s_boot_held{false};
static std::atomic<bool> s_long_press_fired{false};

static bool DispatchBootAction(BootKeyAction action, const char* kind, const char* screen,
                               BootKeyDispatchPhase phase = BootKeyDispatchPhase::Action) {
    if (phase != BootKeyDispatchPhase::PressUp && UiModal::HandleInput(UiModalInput::Other)) {
        if (phase == BootKeyDispatchPhase::PressDown) {
            s_boot_held.store(false);
        }
        return true;
    }
    if (action == nullptr) {
        ESP_LOGD(TAG, "%s no-op (screen=%s)", kind, screen);
        return false;
    }
    if (!action()) {
        ESP_LOGD(TAG, "%s ignored by screen=%s", kind, screen);
        return false;
    }
    ESP_LOGD(TAG, "%s handled by screen=%s", kind, screen);
    return true;
}

static bool OtaBlocks() {
    return OtaUpgradeScreen::IsActive();
}

static void EnterStandbyAsync(void* /*arg*/) {
    LogUploadCancel();
    CloudScreen::StopTransfer();
    CloudScreen::ParkApForStandby();
    StandbyPresentation::Show();
}

void BootKey_OnPressDown() {
    if (OtaBlocks()) {
        return;
    }
    s_boot_held.store(true);
    s_long_press_fired.store(false);
    const char* screen = VkKey_ActiveScreen();
    DispatchBootAction(VkKey_GetBootPressDown(screen), "press-down", screen, BootKeyDispatchPhase::PressDown);
}

void BootKey_OnPressUp() {
    if (OtaBlocks()) {
        s_boot_held.store(false);
        return;
    }
    if (!s_boot_held.exchange(false)) {
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    // 按下已交给业务时，松手仍须结束原操作；新弹窗不能截断录音等资源收尾。
    DispatchBootAction(VkKey_GetBootPressUp(screen), "press-up", screen, BootKeyDispatchPhase::PressUp);
}

void BootKey_OnClick() {
    if (OtaBlocks() || s_long_press_fired.load()) {
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    DispatchBootAction(VkKey_GetBootClick(screen), "short-press", screen);
}

void BootKey_OnDoubleClick() {
    if (OtaBlocks() || s_long_press_fired.load()) {
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    DispatchBootAction(VkKey_GetBootDoubleClick(screen), "double-click", screen);
}

void BootKey_OnLongPress() {
    if (OtaBlocks()) {
        s_long_press_fired.store(true);
        return;
    }
    s_long_press_fired.store(true);
    const char* screen = VkKey_ActiveScreen();
    if (StandbyPresentation::IsActive()) {
        if (StandbyPresentation::HandleBootLongPress()) {
            ESP_LOGD(TAG, "long-press handled by standby");
            return;
        }
    }
    DispatchBootAction(VkKey_GetBootLongPress(screen), "long-press", screen);
}

void PowerKey_OnClick() {
    if (OtaBlocks()) {
        return;
    }
    if (StandbyPresentation::IsActive()) {
        StandbyPresentation::RequestWake();
        return;
    }
    auto& usb = UsbVirtualDisk::GetInstance();
    if (usb.IsGadgetActive() || usb.IsBusy()) {
        ESP_LOGI(TAG, "power standby deferred: USB owns SD");
        return;
    }
    ESP_LOGI(TAG, "power short -> standby");
    PowerPolicy::GetInstance().PreparePowerKeyStandby();
    if (!ScreenLvAsyncUrgent(EnterStandbyAsync)) {
        ScreenLvAsync(EnterStandbyAsync);
    }
}

bool BootKey_IsHeld() {
    return s_boot_held.load();
}

bool BootKey_DidLongPress() {
    return s_long_press_fired.load();
}
