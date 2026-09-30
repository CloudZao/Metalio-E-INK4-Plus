#pragma once

#include "assistant_screen_priv.h"

/** @brief 取字体行高（含行距估算） */
lv_coord_t Assistant_FontLineHeight(const lv_font_t* font);
/** @brief UI 粗体字（fontpack） */
const lv_font_t* Assistant_UiFontBold();
/** @brief 按 variant 名返回字距 */
lv_coord_t Assistant_VariantLetterSpace(const char* variant);
/** @brief 显示空状态全屏提示图 */
void Assistant_ShowIdleHint();
/** @brief 禁用对象滚动条与滚动 */
void Assistant_DisableScroll(lv_obj_t* obj);
/** @brief UI 常规字（fontpack） */
const lv_font_t* Assistant_UiFont();
/** @brief 单码点字形宽度 */
lv_coord_t Assistant_GlyphWidth(const lv_font_t* font, uint32_t cp);
/** @brief 解码空状态提示图到 Image 控件 */
void Assistant_LoadIdleHintImage(lv_obj_t* img);
/** @brief 隐藏空状态提示图 */
void Assistant_HideIdleHint();
/** @brief 按粗体标志选择 UI 字体 */
const lv_font_t* Assistant_FontFor(bool bold);
/** @brief 确保 a2ui 宿主已挂到屏幕 */
void Assistant_EnsureA2ui(lv_obj_t* scr);
