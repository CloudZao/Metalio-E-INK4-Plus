#pragma once

/**
 * A2UI 设备端 LaTeX 数学公式子集：解析 → TeX 盒模型 → LVGL 绝对定位渲染。
 * 字体：fontpack Latin Modern Math @ 18/28/36 bpp2（与 UI 25/30 错开）。
 */

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

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
 * 在 parent 下创建公式控件（单个注册节点；内部子控件不占 A2UI 节点表）。
 * @param latex  LaTeX 字符串，可带或不带 $...$ / $$...$$
 * @param display_style  true=展示式（更大运算符/分式）
 * @param max_width  可用宽度；超出时仍单行绘制（墨水屏不折行公式）
 * @return 根容器，失败返回 NULL
 */
lv_obj_t *a2ui_math_create(lv_obj_t *parent, const char *latex, bool display_style,
                           int32_t max_width);

/**
 * 仅测量排版高度（含上下伸出），供 assistant 分页。失败返回 -1。
 */
int32_t a2ui_math_measure_height(const char *latex, bool display_style, int32_t max_width);

#ifdef __cplusplus
}
#endif
