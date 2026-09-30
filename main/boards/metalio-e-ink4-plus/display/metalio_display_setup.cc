/**
 * @file metalio_display_setup.cc
 * @brief S31：创建 LvAdapterEpdiy
 */

#include "metalio_display_setup.h"

#include "lv_adapter_epdiy.h"

#include <esp_log.h>

#define TAG "MetalioDisplay"

Display* MetalioDisplay_Create(void) {
    auto* disp = new LvAdapterEpdiy();
    if (!disp->Init()) {
        ESP_LOGE(TAG, "LvAdapterEpdiy init fail");
        delete disp;
        return nullptr;
    }
    return disp;
}

void MetalioDisplay_SetSystemReady(Display* display) {
    if (display == nullptr) {
        return;
    }
    static_cast<LvAdapterEpdiy*>(display)->SetSystemReady();
}
