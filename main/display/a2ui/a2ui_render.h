#pragma once

#include "a2ui.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void a2ui_render_init(lv_display_t *disp, const lv_font_t *font_regular,
                      const lv_font_t *font_bold);

/** Host container for updateComponents; NULL = active screen. */
void a2ui_render_set_host(lv_obj_t *host);

/** Parse one A2UI JSON object (null-terminated). Caller holds LVGL lock. */
esp_err_t a2ui_render_handle_json(const char *json);

a2ui_refresh_t a2ui_render_take_pending_refresh(void);

#ifdef __cplusplus
}
#endif
