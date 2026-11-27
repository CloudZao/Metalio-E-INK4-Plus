#include <esp_log.h>
#include "display_ui.h"

static DisplayUiBindings s_bindings;

void DisplayUiConfigure(const DisplayUiBindings& bindings) {
    s_bindings = bindings;
}

lv_obj_t* DisplayUiCreateStartup(bool touch_ready, bool defer_restore, DisplayUiScreenFactory& pending) {
    pending = nullptr;
    if (!s_bindings.create_startup) {
        ESP_LOGE("DisplayUi", "operation=startup result=unconfigured");
        return nullptr;
    }
    return s_bindings.create_startup(touch_ready, defer_restore, pending);
}

bool DisplayUiPreparePowerOff() {
    if (!s_bindings.prepare_power_off) {
        ESP_LOGE("DisplayUi", "operation=power_off result=unconfigured");
        return false;
    }
    return s_bindings.prepare_power_off();
}

bool DisplayUiWaveVisible() {
    return s_bindings.wave_visible && s_bindings.wave_visible();
}
