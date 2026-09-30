#pragma once

// A2UI-lite e-paper UI renderer：不处理传输层，只负责渲染与交互回调。
// 业务层把下行 JSON 交给 a2ui_handle_json()，按钮动作通过 callback 回传。

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    A2UI_REFRESH_NONE = 0,
    A2UI_REFRESH_PARTIAL,
    A2UI_REFRESH_FULL,
} a2ui_refresh_t;

/** Called when a Button action is triggered on device. */
typedef void (*a2ui_action_cb_t)(const char *name, void *user_ctx);

typedef struct {
    lv_display_t *disp;           /**< LVGL display (required) */
    const lv_font_t *font_regular; /**< body / normal weight (required) */
    const lv_font_t *font_bold;    /**< bold weight; falls back to regular */
    const char *http_origin;      /**< e.g. http://192.168.x.x:8765 — rewrite localhost Image.url */
    a2ui_action_cb_t on_action;   /**< optional uplink callback */
    void *user_ctx;               /**< passed to on_action */
} a2ui_config_t;

#define A2UI_CONFIG_DEFAULT()   \
    {                           \
        .disp = NULL,           \
        .font_regular = NULL,   \
        .font_bold = NULL,      \
        .http_origin = NULL,    \
        .on_action = NULL,      \
        .user_ctx = NULL,       \
    }

/** Init renderer (display + optional action callback). No network. */
esp_err_t a2ui_init(const a2ui_config_t *cfg);

/** Update / replace action callback after init. */
void a2ui_set_action_cb(a2ui_action_cb_t cb, void *user_ctx);

/**
 * Render into this host container instead of wiping the active screen.
 * Pass NULL to clear host (falls back to active screen). Caller must hold LVGL lock.
 */
void a2ui_set_host(lv_obj_t *host);

/** Feed one A2UI JSON object (null-terminated) from your transport.
 * Caller must already hold the LVGL / display lock. Does not trigger EPD refresh. */
esp_err_t a2ui_handle_json(const char *json);

/**
 * Consume refresh mode from the last updateComponents.
 * Optional hint for app-level EPD policy; this component does not flush the panel.
 */
a2ui_refresh_t a2ui_take_pending_refresh(void);

#ifdef __cplusplus
}
#endif
