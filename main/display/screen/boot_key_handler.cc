/**
 * @brief BOOT/电源键分发
 */
#include "boot_key_handler.h"

#include "cloud_screen/cloud_screen.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "power_policy.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

#include <atomic>
#include <esp_log.h>

#define TAG "BootKey"

static std::atomic<bool> s_boot_held{false};
static std::atomic<bool> s_long_press_fired{false};

static bool DispatchBootAction(BootKeyAction action, const char* kind, const char* screen) {
    if (action == nullptr) {
        ESP_LOGI(TAG, "%s no-op (screen=%s)", kind, screen);
        return false;
    }
    if (!action()) {
        ESP_LOGI(TAG, "%s ignored by screen=%s", kind, screen);
        return false;
    }
    ESP_LOGI(TAG, "%s handled by screen=%s", kind, screen);
    return true;
}

static bool OtaBlocks() {
    return OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive();
}

static void EnterStandbyAsync(void* /*arg*/) {
    CloudScreen::StopTransfer();
    StandbyScreen::Show();
}

void BootKey_OnPressDown() {
    if (OtaBlocks()) {
        return;
    }
    s_boot_held.store(true);
    s_long_press_fired.store(false);
    const char* screen = VkKey_ActiveScreen();
    DispatchBootAction(VkKey_GetBootPressDown(screen), "press-down", screen);
}

void BootKey_OnPressUp() {
    if (OtaBlocks()) {
        s_boot_held.store(false);
        return;
    }
    s_boot_held.store(false);
    const char* screen = VkKey_ActiveScreen();
    DispatchBootAction(VkKey_GetBootPressUp(screen), "press-up", screen);
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
    if (StandbyScreen::IsActive()) {
        if (StandbyScreen::HandleBootLongPress()) {
            ESP_LOGI(TAG, "long-press handled by standby");
            return;
        }
    }
    DispatchBootAction(VkKey_GetBootLongPress(screen), "long-press", screen);
}

void PowerKey_OnClick() {
    if (OtaBlocks()) {
        return;
    }
    if (StandbyScreen::IsActive()) {
        ESP_LOGI(TAG, "power short ignored (already standby)");
        return;
    }
    ESP_LOGI(TAG, "power short -> standby");
    PowerPolicy::GetInstance().PreparePowerKeyStandby();
    if (!ScreenLvAsyncUrgent(EnterStandbyAsync)) {
        ScreenLvAsync(EnterStandbyAsync);
    }
}

void PowerKey_OnLongPress() {
    ESP_LOGI(TAG, "power long -> RequestPowerOff");
    CloudScreen::StopTransfer();
    PowerPolicy::GetInstance().RequestPowerOff();
}

bool BootKey_IsHeld() {
    return s_boot_held.load();
}

bool BootKey_DidLongPress() {
    return s_long_press_fired.load();
}
