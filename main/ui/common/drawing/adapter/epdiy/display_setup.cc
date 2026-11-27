/**
 * @file display_setup.cc
 * @brief S31：创建 LvAdapterEpdiy
 */

#include <esp_log.h>
#include "display_setup.h"
#include "lv_adapter_epdiy.h"

#define TAG "DisplaySetup"

Display* DisplaySetup_Create(void) {
    auto* disp = new LvAdapterEpdiy();
    if (!disp->Init()) {
        ESP_LOGE(TAG, "LvAdapterEpdiy init fail");
        delete disp;
        return nullptr;
    }
    return disp;
}

void DisplaySetup_SetSystemReady(Display* display) {
    if (display == nullptr) {
        return;
    }
    static_cast<LvAdapterEpdiy*>(display)->SetSystemReady();
}
