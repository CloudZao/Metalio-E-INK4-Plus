#include "display_setup.h"
#include "config.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "sysmgmt/power/policy/power_policy.h"

void TouchInput_AfterWake(void) {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->KickTouchInput();
    }
}

static bool DisplaySetup_OnTouchVirtualKeyEvent(const char* name, TouchVkEvent event, void* /*user_data*/) {
    if (name == nullptr) {
        return false;
    }
    switch (event) {
        case TouchVkEvent::PressDown:
            VkKey_PostTouch(VkTouchPhase::kDown, name);
            break;
        case TouchVkEvent::LongPress:
            VkKey_PostTouch(VkTouchPhase::kLong, name);
            return true;
        case TouchVkEvent::PressUp:
            VkKey_PostTouch(VkTouchPhase::kUp, name);
            break;
        case TouchVkEvent::Click:
            VkKey_PostTouch(VkTouchPhase::kDown, name);
            VkKey_PostTouch(VkTouchPhase::kUp, name);
            break;
    }
    return false;
}

Display* DisplaySetup_Create(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_touch_handle_t touch) {
    Display* display = new LVAdapterDisplay(panel, panel_io, touch, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    if (touch == nullptr) {
        return display;
    }

    static const TouchVirtualKey kTouchKeys[] = {
        // HOME：与 BOOT 共用板型长按阈值 kBootLongPressMs；长按→回系统首页（含百问页）
        {"vk_home", TOUCH_VK_HOME_X, TOUCH_VK_HOME_Y, kBootLongPressMs},
        // prev/next：百问长按→首页/末页；其它页未消费则松手仍走短按
        {"vk_prev", TOUCH_VK_PREV_X, TOUCH_VK_PREV_Y, kBootLongPressMs},
        {"vk_next", TOUCH_VK_NEXT_X, TOUCH_VK_NEXT_Y, kBootLongPressMs},
    };
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RegisterTouchVirtualKeys(kTouchKeys, sizeof(kTouchKeys) / sizeof(kTouchKeys[0]),
                                       DisplaySetup_OnTouchVirtualKeyEvent, nullptr);
    }
    return display;
}
