#pragma once

/**
 * @file ui_scale.h
 * @brief 相对 397 逻辑竖屏（480×800）的同比例缩放
 *
 * 470 可绘区为 DISPLAY_CONTENT_*（已扣面板 inset）。布局尺寸请用 UiSx/UiSy
 * 从 397 源值换算，勿直接抄同像素。
 */

#include "config.h"

#include <lvgl.h>

/** 397 逻辑竖屏宽高（与 CST816S / LVGL 一致） */
constexpr int kUiRefW = 480;
constexpr int kUiRefH = 800;

/** @brief 按宽度比例缩放（水平间距、方图标边长等） */
constexpr lv_coord_t UiSx(int n) {
    return static_cast<lv_coord_t>((n * DISPLAY_CONTENT_W + kUiRefW / 2) / kUiRefW);
}

/** @brief 按高度比例缩放（纵向间距、行高、底栏等） */
constexpr lv_coord_t UiSy(int n) {
    return static_cast<lv_coord_t>((n * DISPLAY_CONTENT_H + kUiRefH / 2) / kUiRefH);
}
