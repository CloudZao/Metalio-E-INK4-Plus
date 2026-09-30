#include "a2ui.h"

#include "a2ui_image.h"
#include "a2ui_internal.h"
#include "a2ui_render.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "a2ui";

static a2ui_action_cb_t s_action_cb;
static void *s_action_ctx;

esp_err_t a2ui_init(const a2ui_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->disp, ESP_ERR_INVALID_ARG, TAG, "disp required");
    ESP_RETURN_ON_FALSE(cfg->font_regular, ESP_ERR_INVALID_ARG, TAG, "font_regular required");
    s_action_cb = cfg->on_action;
    s_action_ctx = cfg->user_ctx;
    a2ui_image_set_http_origin(cfg->http_origin);
    a2ui_render_init(cfg->disp, cfg->font_regular, cfg->font_bold);
    ESP_LOGI(TAG, "renderer ready");
    return ESP_OK;
}

void a2ui_set_action_cb(a2ui_action_cb_t cb, void *user_ctx)
{
    s_action_cb = cb;
    s_action_ctx = user_ctx;
}

void a2ui_set_host(lv_obj_t *host)
{
    a2ui_render_set_host(host);
}

void a2ui_dispatch_action(const char *name)
{
    if (!name || !s_action_cb) {
        return;
    }
    s_action_cb(name, s_action_ctx);
}

esp_err_t a2ui_handle_json(const char *json)
{
    return a2ui_render_handle_json(json);
}

a2ui_refresh_t a2ui_take_pending_refresh(void)
{
    return a2ui_render_take_pending_refresh();
}
