#pragma once

#include <stdbool.h>

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kDisplayUiPortrait = 0,  // 竖屏（逻辑 DISPLAY_CONTENT_W×H，已扣 inset）
    kDisplayUiLandLeft = 1,  // 左横（暂未启用）
    kDisplayUiLandRight = 2, // 右横
    kDisplayUiOrientCount = 3,
};

/**
 * @brief 绑定 LVGL 显示与面板物理尺寸（仅初始化调用一次）
 */
void DisplayUiBind(lv_display_t* disp, int panel_w, int panel_h);

/**
 * @brief 切换 UI 方向并更新 LVGL 逻辑分辨率
 * @return 方向实际变化时为 true
 * @note 须在 LVGL 任务或已持 adapter 锁时调用
 */
bool DisplayUiSetOrient(int orient);

/** @brief 当前 UI 方向 */
int DisplayUiGetOrient(void);

/** @brief 是否为横屏（左横或右横） */
bool DisplayUiIsLandscape(void);

/** @brief 当前逻辑宽 */
int DisplayUiLogW(void);

/** @brief 当前逻辑高 */
int DisplayUiLogH(void);

/**
 * @brief 逻辑坐标 → 面板物理坐标
 */
void DisplayUiLogicalToPanel(int lx, int ly, int* px, int* py);

/**
 * @brief CST816S 原生竖屏坐标 → 当前逻辑坐标（盖板键仍用原生坐标命中）
 */
void DisplayUiTouchToLogical(int tx, int ty, int* lx, int* ly);

#ifdef __cplusplus
}
#endif
