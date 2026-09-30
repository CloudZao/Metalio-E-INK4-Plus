#pragma once

#include "lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Flash fontpack → LVGL 字体桥。
 * 需先烧录 fonts.fontpack 到 font_data 分区（含 25/30@2 等）。
 * 失败时返回 nullptr。
 */
const lv_font_t* fontpack_lv_font_get(uint16_t size, uint16_t bpp);

/**
 * 全局 UI 默认字：fontpack **30@2**（397 为 25@2；字库无中间档，取最接近同比例的一档）。
 * 可重复调用；首次成功后缓存。失败返回 nullptr。
 */
const lv_font_t* fontpack_lv_font_ui(void);

/** 若 font 由 fontpack_lv_font_get 创建，返回其 bpp；否则返回 0。 */
uint16_t fontpack_lv_font_bpp(const lv_font_t* font);

/** 若尚未 mmap，尝试 font_loader_init_flash；可重复调用。 */
bool fontpack_lv_ensure_ready(void);

#ifdef __cplusplus
}
#endif
