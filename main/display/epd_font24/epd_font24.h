/**
 * @file epd_font24.h
 * @brief ebook 同源 24×24 1bpp 点阵；全库在 font_data 分区 mmap
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_FONT24_SIZE 24 // 字号边长
#define EPD_FONT24_ASCII_ADVANCE 14 // ASCII 字距（对齐 ebook）
#define EPD_FONT24_PARTITION_LABEL "font_data" // 分区名
#define EPD_LOGICAL_W 684 // 竖屏逻辑宽（与 ebook 一致）
#define EPD_LOGICAL_H 1216 // 竖屏逻辑高

/** 竖屏逻辑坐标 → 原生横屏 FB（EPD_ROT_LANDSCAPE 下直接 epd_draw_pixel） */
static inline void epd_logical_to_physical(int xL, int yL, int* xP, int* yP) {
    *xP = yL;
    *yP = EPD_LOGICAL_W - 1 - xL;
}

/**
 * @brief 从 font_data 分区 mmap 字库
 * @return true 成功（magic/版本校验通过）
 */
bool epd_font24_init(void);

/** @brief 字库是否已就绪 */
bool epd_font24_ready(void);

/**
 * @brief 在 EPDiy FB 上画单个字符（逻辑坐标）
 * @param color 黑 0x00 / 白 0xF0
 */
void epd_font24_draw_char(uint8_t* fb, int x, int y, uint16_t unicode, uint8_t color);

/**
 * @brief 画字符；scale>1 时每点用逻辑方块填充（对照 1px 路径）
 */
void epd_font24_draw_char_scaled(uint8_t* fb, int x, int y, uint16_t unicode, uint8_t color,
                                 int scale);

/**
 * @brief 画 UTF-8 字符串
 * @return 绘制后光标 X
 */
int epd_font24_draw_string(uint8_t* fb, int x, int y, const char* text, uint8_t color);

/**
 * @brief 画 UTF-8 字符串（可放大方块）
 * @return 绘制后光标 X
 */
int epd_font24_draw_string_scaled(uint8_t* fb, int x, int y, const char* text, uint8_t color,
                                  int scale);

/**
 * @brief 水平居中画 UTF-8 字符串
 */
void epd_font24_draw_string_centered(uint8_t* fb, int cx, int y, const char* text, uint8_t color,
                                     int fb_w);

/**
 * @brief 水平居中画 UTF-8 字符串（可放大方块）
 */
void epd_font24_draw_string_centered_scaled(uint8_t* fb, int cx, int y, const char* text,
                                            uint8_t color, int fb_w, int scale);

#ifdef __cplusplus
}
#endif
