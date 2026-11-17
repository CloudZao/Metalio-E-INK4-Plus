#pragma once

#include <stdbool.h>

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kDisplayUiPortrait = 0,  // 竖屏（逻辑可绘区域已扣边距）
    kDisplayUiLandLeft = 1,  // 左横
    kDisplayUiLandRight = 2, // 右横
    kDisplayUiPortraitDown = 3, // 倒置竖屏，保持旧方向数值兼容
    kDisplayUiOrientCount = 4,
};

enum {
    kDisplayUiOffsetCount = 4, // 边序为上、下、左、右
    kDisplayUiOffsetMax = 20, // 单项偏移绝对值上限，单位为像素
    kDisplayUiOffsetMin = -kDisplayUiOffsetMax, // 单项偏移下限
};

/** @brief 获取当前方向的 UI 平移量，方向顺序为上、下、左、右 */
int DisplayUiOffset(int side);

/**
 * @brief 即时设置指定方向的 UI 平移量并请求重绘，保持逻辑分辨率不变
 * @note 须在 LVGL 任务或已持 adapter 锁时调用，不负责持久化
 * @return 有效平移量发生变化时返回 true
 */
bool DisplayUiSetOffset(int side, int value);

/**
 * @brief 恢复已保存的默认平移量并请求重绘，保持逻辑分辨率不变
 * @note 须在 LVGL 任务或已持 adapter 锁时调用，不负责持久化
 * @return 任一平移量发生变化时返回 true
 */
bool DisplayUiResetOffsets(void);

/**
 * @brief 将当前四个方向的平移量设为默认值，仅更新 RAM
 * @note 须在 LVGL 任务或已持 adapter 锁时调用，持久化由调用方负责
 */
void DisplayUiSetOffsetDefaults(void);

/** @brief 获取水平净偏移，正值向右、负值向左 */
int DisplayUiOffsetX(void);

/** @brief 获取垂直净偏移，正值向下、负值向上 */
int DisplayUiOffsetY(void);

/** @brief 获取普通 UI 的固定竖屏可绘宽度 */
int DisplayUiContentW(void);

/** @brief 获取普通 UI 的固定竖屏可绘高度 */
int DisplayUiContentH(void);

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

/**
 * @brief 切换全屏绘制，待机与关机画面不应用普通 UI 内缩与平移
 * @note 须在 LVGL 任务或已持 adapter 锁时调用
 */
void DisplayUiSetFullScreen(bool full_screen);

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
 * @brief 触摸驱动归一化的原生竖屏坐标 → 当前逻辑坐标（盖板键仍用原生坐标命中）
 */
void DisplayUiTouchToLogical(int tx, int ty, int* lx, int* ly);

#ifdef __cplusplus
}
#endif
