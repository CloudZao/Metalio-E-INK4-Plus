#pragma once

/**
 * A2UI 设备端 LaTeX 数学公式子集：解析 → TeX 盒模型 → LVGL 绝对定位渲染。
 * 字体：fontpack Latin Modern Math @ 18/28/36 bpp2（与 UI 25/30 错开）。
 */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认正文/脚本字号（须已打进 fontpack）。 */
#ifndef A2UI_MATH_SIZE_TEXT
#define A2UI_MATH_SIZE_TEXT 28
#endif
#ifndef A2UI_MATH_SIZE_SCRIPT
#define A2UI_MATH_SIZE_SCRIPT 18
#endif
#ifndef A2UI_MATH_SIZE_DISPLAY
#define A2UI_MATH_SIZE_DISPLAY 36
#endif
#ifndef A2UI_MATH_BPP
#define A2UI_MATH_BPP 2
#endif

/**
 * @brief 在父容器下创建单行公式控件，解析失败时回退显示原文。
 * @param latex LaTeX 文本，可包含行内或展示式公式分隔符。
 * @param display_style 是否使用展示式公式排版。
 * @param max_width 公式可用宽度，超出时保持单行绘制。
 * @return 公式根控件，资源不可用时返回空指针。
 */
lv_obj_t* a2ui_math_create(lv_obj_t* parent, const char* latex, bool display_style, int32_t max_width);

/** @brief 测量公式排版高度，包含上下延伸部分；失败返回 -1。 */
int32_t a2ui_math_measure_height(const char* latex, bool display_style, int32_t max_width);

#ifdef __cplusplus
}
#endif
