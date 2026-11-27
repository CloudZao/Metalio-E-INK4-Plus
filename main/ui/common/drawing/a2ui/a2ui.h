#pragma once

// A2UI 电子纸渲染接口，负责内容渲染及交互回调。
// 业务层交付下行 JSON，按钮动作通过已注册回调返回。

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

// 设备按钮动作回调，携带动作名称及调用方上下文。
typedef void (*a2ui_action_cb_t)(const char* name, void* user_ctx);

typedef struct {
    lv_display_t* disp;             // LVGL 显示对象，必填
    const lv_font_t* font_regular;  // 正文字体，必填
    const lv_font_t* font_bold;     // 强调字体，未指定时使用正文字体
    const char* http_origin;        // 图像来源基址，可为空
    a2ui_action_cb_t on_action;     // 可选的动作回调
    void* user_ctx;                 // 动作回调的调用方上下文
} a2ui_config_t;

#define A2UI_CONFIG_DEFAULT() \
    {                         \
        .disp = NULL,         \
        .font_regular = NULL, \
        .font_bold = NULL,    \
        .http_origin = NULL,  \
        .on_action = NULL,    \
        .user_ctx = NULL,     \
    }

/**
 * @brief 初始化显示、字体和动作回调；不建立网络连接。
 * @note 显示、字体及回调上下文由调用方持有，渲染期间须保持有效。
 * @return 配置有效返回 ESP_OK，否则返回参数错误。
 */
esp_err_t a2ui_init(const a2ui_config_t* cfg);

/** @brief 替换动作回调及其调用方上下文。 */
void a2ui_set_action_cb(a2ui_action_cb_t cb, void* user_ctx);

/**
 * @brief 指定后续内容更新的父容器；空指针恢复使用活动屏幕。
 * @note 调用方须持有 LVGL 显示锁。
 */
void a2ui_set_host(lv_obj_t* host);

/**
 * @brief 处理以空字符结束的单个 A2UI JSON 对象。
 * @note 调用方须持有 LVGL 显示锁；面板刷新由上层调度。
 */
esp_err_t a2ui_handle_json(const char* json);

/**
 * @brief 获取待处理的刷新提示，并将后续提示恢复为默认局刷。
 * @note 仅向上层提供刷新策略提示，不直接提交面板刷新。
 */
a2ui_refresh_t a2ui_take_pending_refresh(void);

#ifdef __cplusplus
}
#endif
