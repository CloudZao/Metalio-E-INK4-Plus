/**
 * @file epd_i1_glyph_thin.h
 * @brief I1 字形落墨：CrossPoint BW「非白即黑」+ 可选真 4 灰 sidecar
 *
 * aa 关：非白即黑实心落墨（局刷 I1）
 * aa 开：I1 仍实心；覆盖度经 epd_i1_glyph_aa_plot 写入 L8 sidecar
 *
 * @note 本头被 lvgl 编译，禁止直接调用 main 内 epd_gray_aa_*（否则 libmain
 *       未抽出时链接失败）。plot 实现放在 BookPrefs 等同必链入的 .o。
 */

#ifndef EPD_I1_GLYPH_THIN_H
#define EPD_I1_GLYPH_THIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 0=BW 局刷；非0=真 4 灰 AA。由 BookReaderPrefs 同步。 */
extern volatile int epd_i1_glyph_aa_enabled;

/** @brief aa 开时写 sidecar；实现在 book_reader_prefs（保证链入） */
void epd_i1_glyph_aa_plot(int32_t abs_x, int32_t abs_y, uint8_t mask_val);

static inline void epd_i1_glyph_set_aa(int enabled)
{
    epd_i1_glyph_aa_enabled = enabled != 0 ? 1 : 0;
}

/**
 * A2 mask：0 / 85 / 170 / 255
 * @param abs_x,abs_y LVGL 逻辑坐标
 * @return 1 落墨到 I1，0 不画
 */
static inline int epd_i1_glyph_mask_hit(uint8_t mask_val, int32_t abs_x, int32_t abs_y)
{
    if (mask_val == 0) {
        return 0;
    }
    if (epd_i1_glyph_aa_enabled) {
        epd_i1_glyph_aa_plot(abs_x, abs_y, mask_val);
    }
    /* CrossPoint BW：非白即黑（I1 实心底） */
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* EPD_I1_GLYPH_THIN_H */
