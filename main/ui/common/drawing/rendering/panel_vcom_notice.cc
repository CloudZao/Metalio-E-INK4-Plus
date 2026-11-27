#include <cstdint>
#include <cstring>
#include <esp_log.h>
#include "panel_vcom_notice.h"
#include "assets/lang_screen.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

void UiNotifyPanelVcom(bool write, const char* error) {
    const uintptr_t notice = error && !strcmp(error, "INVALID_VCOM") ? 5 : (error ? (write ? 4 : 3) : (write ? 2 : 1));
    if (!ScreenLvAsync(
            [](void* data) {
                const auto kind = reinterpret_cast<uintptr_t>(data);
                const char* text = kind == 5   ? LangString_SETTINGS_VCOM_INVALID
                                   : kind == 4 ? LangString_SETTINGS_VCOM_WRITE_FAILED
                                   : kind == 3 ? LangString_SETTINGS_VCOM_READ_FAILED
                                   : kind == 2 ? LangString_SETTINGS_VCOM_WRITE_OK
                                               : LangString_SETTINGS_VCOM_READ_OK;
                ScreenShowToast(text);
            },
            reinterpret_cast<void*>(notice))) {
        ESP_LOGW("PanelVcomUi", "result notification enqueue failed");
    }
}
