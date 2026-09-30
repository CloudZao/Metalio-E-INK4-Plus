#include "vk_key_handler.h"

#include "vk_page_repeat.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "battery_uv_lockout.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"


static constexpr const char* TAG = "VkKey";
static constexpr const char* kNoneScreen = "none";
static constexpr const char* kHomeScreen = "home";
static constexpr int kMaxStack = 8;
static constexpr int kMaxRegistry = 24;

static const char* s_stack[kMaxStack] = {};
static int s_depth = 0;

struct VkKeyRegistryEntry {
    const char* name = nullptr;
    ScreenFactory factory = nullptr;
    VkKeyHandler on_key = nullptr;
    BootKeyAction on_boot_click = nullptr;
    BootKeyAction on_boot_long_press = nullptr;
    BootKeyAction on_boot_press_down = nullptr;
    BootKeyAction on_boot_press_up = nullptr;
    VkKeyHandler on_key_long_press = nullptr;
    VkKeyHandler on_key_press_up = nullptr;
    BootKeyAction on_boot_double_click = nullptr;
};

static VkKeyRegistryEntry s_registry[kMaxRegistry] = {};
static int s_registry_count = 0;

static void StackPush(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (s_depth < kMaxStack) {
        s_stack[s_depth++] = name;
    } else {
        s_stack[kMaxStack - 1] = name;
        ESP_LOGW(TAG, "screen stack full, replace top with %s", name);
    }
}

static void StackRemoveTopmost(const char* name) {
    if (name == nullptr || s_depth <= 0) {
        return;
    }
    for (int i = s_depth - 1; i >= 0; --i) {
        if (std::strcmp(s_stack[i], name) == 0) {
            for (int j = i; j < s_depth - 1; ++j) {
                s_stack[j] = s_stack[j + 1];
            }
            --s_depth;
            s_stack[s_depth] = nullptr;
            return;
        }
    }
}

static const char* StackTop() {
    return s_depth > 0 ? s_stack[s_depth - 1] : kNoneScreen;
}

VkKeyRegistryEntry* FindRegistry(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < s_registry_count; ++i) {
        if (s_registry[i].name != nullptr && std::strcmp(s_registry[i].name, name) == 0) {
            return &s_registry[i];
        }
    }
    return nullptr;
}

static void UpsertRegistry(const char* name, const VkKeyScreenDesc& desc) {
    if (name == nullptr) {
        return;
    }
    if (auto* e = FindRegistry(name)) {
        e->factory = desc.factory;
        e->on_key = desc.on_key;
        e->on_boot_click = desc.on_boot_click;
        e->on_boot_long_press = desc.on_boot_long_press;
        e->on_boot_press_down = desc.on_boot_press_down;
        e->on_boot_press_up = desc.on_boot_press_up;
        e->on_key_long_press = desc.on_key_long_press;
        e->on_key_press_up = desc.on_key_press_up;
        e->on_boot_double_click = desc.on_boot_double_click;
        return;
    }
    if (s_registry_count >= kMaxRegistry) {
        ESP_LOGW(TAG, "registry full, drop %s", name);
        return;
    }
    s_registry[s_registry_count++] = VkKeyRegistryEntry{
        name,
        desc.factory,
        desc.on_key,
        desc.on_boot_click,
        desc.on_boot_long_press,
        desc.on_boot_press_down,
        desc.on_boot_press_up,
        desc.on_key_long_press,
        desc.on_key_press_up,
        desc.on_boot_double_click};
}

static void OnScreenLifecycleEvent(lv_event_t* e) {
    const char* name = static_cast<const char*>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        VkKey_OnScreenLifecycle(name, VK_SCREEN_LIFECYCLE_LOAD);
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        VkKey_OnScreenLifecycle(name, VK_SCREEN_LIFECYCLE_UNLOAD);
    }
}

static void DispatchDefault(const char* key_name, const char* screen) {
    if (std::strcmp(key_name, "vk_home") == 0) {
        if (std::strcmp(screen, kHomeScreen) == 0) {
            ESP_LOGI(TAG, "vk_home no-op (already home)");
            return;
        }
        ScreenRequestHome();
        return;
    }

    if (std::strcmp(key_name, "vk_prev") == 0) {
        if (std::strcmp(screen, kHomeScreen) == 0) {
            ESP_LOGI(TAG, "vk_prev no-op (already home)");
            return;
        }
        ESP_LOGI(TAG, "vk_prev -> navigate back");
        ScreenNavigateBack();
        return;
    }

    if (std::strcmp(key_name, "vk_next") == 0) {
        ESP_LOGD(TAG, "vk_next no-op (default)");
        return;
    }

    ESP_LOGW(TAG, "unknown key %s", key_name);
}


void VkKey_OnScreenLifecycle(const char* name, vk_screen_lifecycle_t event) {
    if (name == nullptr || name[0] == '\0') {
        name = kNoneScreen;
    }

    if (event == VK_SCREEN_LIFECYCLE_LOAD) {
        StackPush(name);
        ESP_LOGD(TAG, "active_screen -> %s (load %s, depth=%d)", StackTop(), name, s_depth);
        return;
    }

    StackRemoveTopmost(name);
    ESP_LOGD(TAG, "active_screen -> %s (unload %s, depth=%d)", StackTop(), name, s_depth);
    VkPageRepeatStop();
}

const char* VkKey_ActiveScreen() {
    return StackTop();
}

ScreenFactory VkKey_GetScreenFactory(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->factory;
    }
    return nullptr;
}

void VkKey_SetScreenFactory(const char* name, ScreenFactory factory) {
    if (auto* e = FindRegistry(name)) {
        e->factory = factory;
    }
}

BootKeyAction VkKey_GetBootClick(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->on_boot_click;
    }
    return nullptr;
}

BootKeyAction VkKey_GetBootLongPress(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->on_boot_long_press;
    }
    return nullptr;
}

BootKeyAction VkKey_GetBootPressDown(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->on_boot_press_down;
    }
    return nullptr;
}

BootKeyAction VkKey_GetBootPressUp(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->on_boot_press_up;
    }
    return nullptr;
}

BootKeyAction VkKey_GetBootDoubleClick(const char* name) {
    if (auto* e = FindRegistry(name)) {
        return e->on_boot_double_click;
    }
    return nullptr;
}

void VkKey_AttachScreen(lv_obj_t* scr, const char* name) {
    VkKey_AttachScreen(scr, name, VkKeyScreenDesc{});
}

void VkKey_AttachScreen(lv_obj_t* scr, const char* name, const VkKeyScreenDesc& desc) {
    if (scr == nullptr || name == nullptr) {
        return;
    }
    UpsertRegistry(name, desc);
    lv_obj_add_event_cb(scr, OnScreenLifecycleEvent, LV_EVENT_SCREEN_LOADED,
                        const_cast<char*>(name));
    lv_obj_add_event_cb(scr, OnScreenLifecycleEvent, LV_EVENT_SCREEN_UNLOADED,
                        const_cast<char*>(name));
}

void VkKey_Dispatch(const char* key_name) {
    if (key_name == nullptr) {
        return;
    }
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGI(TAG, "%s blocked (ota upgrade active)", key_name);
        return;
    }
    if (StandbyScreen::IsActive()) {
        ESP_LOGI(TAG, "%s blocked (standby overlay active)", key_name);
        return;
    }

    const char* screen = StackTop();
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "%s on screen=%s (depth=%d) begin", key_name, screen, s_depth);

    if (auto* e = FindRegistry(screen)) {
        if (e->on_key != nullptr) {
            const bool consumed = e->on_key(key_name);
            const int us = static_cast<int>(esp_timer_get_time() - t0);
            ESP_LOGI(TAG, "%s handler %s in %d us (screen=%s)", key_name,
                     consumed ? "consumed" : "fallthrough", us, screen);
            if (us > 20000) {
                ESP_LOGW(TAG, "%s handler slow %d us — likely sync LVGL on touch_feed", key_name,
                         us);
            }
            if (consumed) {
                return;
            }
        }
    }

    DispatchDefault(key_name, screen);
    ESP_LOGI(TAG, "%s default done in %d us", key_name,
             static_cast<int>(esp_timer_get_time() - t0));
}

bool VkKey_OnLongPress(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    if (BatteryUvLockout_IsActive()) {
        return true;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGI(TAG, "%s long-press blocked (ota upgrade active)", key_name);
        return true;  // 已消费，避免下层误处理
    }
    if (StandbyScreen::IsActive()) {
        ESP_LOGI(TAG, "%s long-press blocked (standby overlay active)", key_name);
        return false;
    }
    const char* screen = StackTop();
    ESP_LOGI(TAG, "%s long-press on screen=%s", key_name, screen);
    if (auto* e = FindRegistry(screen)) {
        if (e->on_key_long_press != nullptr && e->on_key_long_press(key_name)) {
            ESP_LOGI(TAG, "%s long-press handled by screen=%s", key_name, screen);
            return true;
        }
    }
    // 默认：长按 vk_home 一键回系统首页（页未消费时；OTA 已拦截）
    if (std::strcmp(key_name, "vk_home") == 0) {
        if (std::strcmp(screen, kHomeScreen) == 0) {
            ESP_LOGI(TAG, "vk_home long-press no-op (already home)");
            return true;  // 吞松手 Click
        }
        ESP_LOGI(TAG, "vk_home long-press -> go home");
        ScreenRequestHome();
        return true;
    }
    ESP_LOGI(TAG, "%s long-press no-op (screen=%s)", key_name, screen);
    return false;
}

bool VkKey_OnPressUp(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    if (BatteryUvLockout_IsActive()) {
        return true;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        return true;
    }
    if (StandbyScreen::IsActive()) {
        return false;
    }
    const char* screen = StackTop();
    ESP_LOGD(TAG, "%s press-up on screen=%s", key_name, screen);
    if (auto* e = FindRegistry(screen)) {
        if (e->on_key_press_up != nullptr && e->on_key_press_up(key_name)) {
            ESP_LOGI(TAG, "%s press-up handled by screen=%s", key_name, screen);
            return true;
        }
    }
    return false;
}
