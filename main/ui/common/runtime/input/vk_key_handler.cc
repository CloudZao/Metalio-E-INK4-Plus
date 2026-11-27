#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include "ui/common/runtime/input/binding_dispatch.h"
#include "sysmgmt/update/update_presentation.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/vk_page_repeat.h"

static constexpr const char* TAG = "VkKey";
static constexpr const char* kNoneScreen = "none";
static constexpr const char* kHomeScreen = "home";
static constexpr int kMaxStack = 8;
static constexpr int kMaxRegistry = 24;

static const char* s_stack[kMaxStack] = {};
static int s_depth = 0;
static std::atomic<uint32_t> s_screen_epoch{0};

struct VkKeyHandlerTouchEdge {
    VkTouchPhase phase;
    uint8_t key;
    uint32_t epoch;
};

struct VkKeyHandlerTouchMailbox {
    StaticQueue_t storage{};
    uint8_t bytes[24 * sizeof(VkKeyHandlerTouchEdge)]{};
    QueueHandle_t queue = xQueueCreateStatic(24, sizeof(VkKeyHandlerTouchEdge), bytes, &storage);
    std::atomic<bool> overflow{false};
};

static VkKeyHandlerTouchMailbox& TouchQueue() {
    static VkKeyHandlerTouchMailbox mailbox;
    return mailbox;
}

static constexpr const char* kTouchKeys[] = {"vk_home", "vk_prev", "vk_next"};

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
    bool (*volume_keys_turn_pages)() = nullptr;
    bool (*on_page_key)(const char*, bool) = nullptr;
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

static VkKeyRegistryEntry* FindRegistry(const char* name) {
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
        e->volume_keys_turn_pages = desc.volume_keys_turn_pages;
        e->on_page_key = desc.on_page_key;
        return;
    }
    if (s_registry_count >= kMaxRegistry) {
        ESP_LOGW(TAG, "registry full, drop %s", name);
        return;
    }
    s_registry[s_registry_count++] = VkKeyRegistryEntry{name,
                                                        desc.factory,
                                                        desc.on_key,
                                                        desc.on_boot_click,
                                                        desc.on_boot_long_press,
                                                        desc.on_boot_press_down,
                                                        desc.on_boot_press_up,
                                                        desc.on_key_long_press,
                                                        desc.on_key_press_up,
                                                        desc.on_boot_double_click,
                                                        desc.volume_keys_turn_pages,
                                                        desc.on_page_key};
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
            ESP_LOGD(TAG, "vk_home no-op (already home)");
            return;
        }
        ScreenNavigateBack();
        return;
    }

    if (std::strcmp(key_name, "vk_prev") == 0) {
        if (std::strcmp(screen, kHomeScreen) == 0) {
            ESP_LOGD(TAG, "vk_prev no-op (already home)");
            return;
        }
        ESP_LOGD(TAG, "vk_prev -> navigate back");
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
    s_screen_epoch.fetch_add(1, std::memory_order_acq_rel);
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

bool VkKey_VolumeKeysTurnPages() {
    const auto* entry = FindRegistry(StackTop());
    return entry && entry->volume_keys_turn_pages && entry->volume_keys_turn_pages();
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
    lv_obj_add_event_cb(scr, OnScreenLifecycleEvent, LV_EVENT_SCREEN_LOADED, const_cast<char*>(name));
    lv_obj_add_event_cb(scr, OnScreenLifecycleEvent, LV_EVENT_SCREEN_UNLOADED, const_cast<char*>(name));
}

void VkKey_Dispatch(const char* key_name) {
    if (key_name == nullptr) {
        return;
    }
    if (OtaUpgradeScreen::IsActive()) {
        ESP_LOGD(TAG, "%s blocked (ota upgrade active)", key_name);
        return;
    }
    if (StandbyPresentation::IsActive()) {
        ESP_LOGD(TAG, "%s blocked (standby overlay active)", key_name);
        return;
    }
    const bool back = std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_home") == 0;
    if (UiModal::HandleInput(back ? UiModalInput::Back : UiModalInput::Other)) {
        return;
    }
    if (std::strcmp(key_name, "vk_home") == 0) {
        ScreenCommon_CancelPendingNavigation();
    }

    const char* screen = StackTop();
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGD(TAG, "%s on screen=%s (depth=%d) begin", key_name, screen, s_depth);

    if (auto* e = FindRegistry(screen)) {
        if (e->on_key != nullptr) {
            const bool consumed = e->on_key(key_name);
            const int us = static_cast<int>(esp_timer_get_time() - t0);
            ESP_LOGD(TAG, "%s handler %s in %d us (screen=%s)", key_name, consumed ? "consumed" : "fallthrough", us,
                     screen);
            if (us > 20000) {
                ESP_LOGW(TAG, "%s UI handler slow %d us", key_name, us);
            }
            if (consumed) {
                return;
            }
        }
    }

    DispatchDefault(key_name, screen);
    ESP_LOGD(TAG, "%s default done in %d us", key_name, static_cast<int>(esp_timer_get_time() - t0));
}

bool VkKey_OnLongPress(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    if (OtaUpgradeScreen::IsActive()) {
        ESP_LOGD(TAG, "%s long-press blocked (ota upgrade active)", key_name);
        return true;  // 已消费，避免下层误处理
    }
    if (StandbyPresentation::IsActive()) {
        ESP_LOGD(TAG, "%s long-press blocked (standby overlay active)", key_name);
        return false;
    }
    if (std::strcmp(key_name, "vk_home") == 0) {
        ScreenCommon_CancelPendingNavigation();
    }
    if (UiModal::HandleInput(UiModalInput::Other)) {
        return true;
    }
    const char* screen = StackTop();
    ESP_LOGD(TAG, "%s long-press on screen=%s", key_name, screen);
    if (auto* e = FindRegistry(screen)) {
        if (e->on_key_long_press != nullptr && e->on_key_long_press(key_name)) {
            ESP_LOGD(TAG, "%s long-press handled by screen=%s", key_name, screen);
            return true;
        }
    }
    // 默认：长按 vk_home 一键回系统首页（页未消费时；OTA 已拦截）
    if (std::strcmp(key_name, "vk_home") == 0) {
        if (std::strcmp(screen, kHomeScreen) == 0) {
            ESP_LOGD(TAG, "vk_home long-press no-op (already home)");
            return true;  // 吞松手 Click
        }
        ESP_LOGD(TAG, "vk_home long-press -> go home");
        ScreenRequestHome();
        return true;
    }
    ESP_LOGD(TAG, "%s long-press no-op (screen=%s)", key_name, screen);
    return false;
}

bool VkKey_OnPressUp(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    if (OtaUpgradeScreen::IsActive()) {
        return true;
    }
    if (StandbyPresentation::IsActive()) {
        return false;
    }
    if (UiModal::HandleInput(UiModalInput::Other)) {
        return true;
    }
    const char* screen = StackTop();
    ESP_LOGD(TAG, "%s press-up on screen=%s", key_name, screen);
    if (auto* e = FindRegistry(screen)) {
        if (e->on_key_press_up != nullptr && e->on_key_press_up(key_name)) {
            ESP_LOGD(TAG, "%s press-up handled by screen=%s", key_name, screen);
            return true;
        }
    }
    return false;
}

void VkKey_PostTouch(VkTouchPhase phase, const char* key_name) {
    if (!key_name) {
        return;
    }
    for (uint8_t key = 0; key < 3; ++key) {
        if (std::strcmp(key_name, kTouchKeys[key]) != 0) {
            continue;
        }
        const VkKeyHandlerTouchEdge edge{phase, key, s_screen_epoch.load(std::memory_order_acquire)};
        if (xQueueSend(TouchQueue().queue, &edge, 0) != pdTRUE) {
            TouchQueue().overflow.store(true);
        }
        return;
    }
}

void VkKey_DrainTouch() {
    auto& mailbox = TouchQueue();
    static int held_key = -1;
    static uint32_t held_epoch = 0;
    static bool long_consumed = false;
    if (mailbox.overflow.exchange(false)) {
        BindingDispatch(BindingKey::Home, InputCancel);
        BindingDispatch(BindingKey::Previous, InputCancel);
        BindingDispatch(BindingKey::Next, InputCancel);
        held_key = -1;
        xQueueReset(mailbox.queue);
        static int64_t last_log = 0;
        const auto now = esp_timer_get_time();
        if (last_log == 0 || now - last_log >= 5000000) {
            ESP_LOGW(TAG, "touch edge queue full; cancelled incomplete press");
            last_log = now;
        }
        return;
    }
    VkKeyHandlerTouchEdge edge{};
    for (unsigned count = 0; count < 24 && xQueueReceive(mailbox.queue, &edge, 0) == pdTRUE; ++count) {
        if (edge.epoch != s_screen_epoch.load(std::memory_order_acquire)) {
            const BindingKey stale_key = edge.key == 0 ? BindingKey::Home : edge.key == 1 ? BindingKey::Previous : BindingKey::Next;
            BindingDispatch(stale_key, InputCancel);
            held_key = -1;
            continue;
        }
        const BindingKey key = edge.key == 0 ? BindingKey::Home : edge.key == 1 ? BindingKey::Previous : BindingKey::Next;
        if (edge.phase == VkTouchPhase::kDown) {
            held_key = edge.key;
            held_epoch = edge.epoch;
            long_consumed = false;
            BindingDispatch(key, InputPressDown);
        } else if (held_key == edge.key && held_epoch == edge.epoch) {
            if (edge.phase == VkTouchPhase::kLong) {
                BindingDispatch(key, InputLongPress);
                long_consumed = true;
                HapticPulseIfEnabled();
            } else {
                BindingDispatch(key, InputPressUp);
                if (!long_consumed) {
                    BindingDispatch(key, InputClick);
                }
                held_key = -1;
            }
        }
    }
}

uint32_t VkKey_InputEpoch() {
    return s_screen_epoch.load(std::memory_order_acquire);
}

bool VkKey_DispatchPage(const char* key_name, bool long_press) {
    if (!key_name || (std::strcmp(key_name, "vk_prev") != 0 && std::strcmp(key_name, "vk_next") != 0)) {
        return false;
    }
    if (auto* entry = FindRegistry(StackTop())) {
        return entry->on_page_key && entry->on_page_key(key_name, long_press);
    }
    return false;
}
