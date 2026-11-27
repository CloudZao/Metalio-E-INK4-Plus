#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "a2ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 绑定显示和字体并重置父容器；无强调字体时使用正文字体。 */
void a2ui_render_init(lv_display_t* disp, const lv_font_t* font_regular, const lv_font_t* font_bold);

/** @brief 设置内容更新的父容器，空指针时使用活动屏幕。 */
void a2ui_render_set_host(lv_obj_t* host);

/** @brief 在持有 LVGL 锁时解析并应用单个 A2UI JSON 对象。 */
esp_err_t a2ui_render_handle_json(const char* json);

/** @brief 获取待处理的刷新提示，并恢复默认局刷提示。 */
a2ui_refresh_t a2ui_render_take_pending_refresh(void);

#ifdef __cplusplus
}
#endif
