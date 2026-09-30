#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** epdfont v1 — SD 卡轻量位图字体（索引常驻，位图按需/按页加载）。 */

#define EPDFONT_MAGIC "EPDFONT\0"
#define EPDFONT_VERSION 1

/** header.flags 低 4 位为 bpp（1 或 2）。 */
#define EPDFONT_FLAG_BPP_MASK 0x000F

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint16_t version;
    uint16_t flags;
    uint16_t line_height;
    uint16_t base_line;
    int16_t ascender;
    int16_t descender;
    uint32_t interval_count;
    uint32_t glyph_count;
    uint32_t intervals_offset;
    uint32_t glyphs_offset;
    uint32_t bitmaps_offset;
    uint8_t reserved[24];
} epdfont_header_t;

typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t glyph_index;
} epdfont_interval_t;

/** adv_w / ofs 均为像素（非 LVGL 8.4 定点）。 */
typedef struct {
    uint8_t width;
    uint8_t height;
    uint16_t adv_w;
    int16_t ofs_x;
    int16_t ofs_y;
    uint16_t bmp_len;
    uint32_t bmp_off;
    uint16_t reserved;
} epdfont_glyph_t;
#pragma pack(pop)

typedef struct epdfont_t epdfont_t;

/**
 * 打开 SD 上的 .ef（POSIX 路径，如 /sdcard/metalio/e-ink/fonts/misans_25.ef）。
 * 成功时返回句柄；失败返回 NULL。
 */
epdfont_t* epdfont_open(const char* path);

void epdfont_close(epdfont_t* font);

/** 对接 LVGL 的字体对象（生命周期与 epdfont_t 相同）。 */
const lv_font_t* epdfont_get_lv_font(const epdfont_t* font);

uint8_t epdfont_get_bpp(const epdfont_t* font);
uint32_t epdfont_glyph_count(const epdfont_t* font);

/**
 * 渲染前预热：从 UTF-8 文本收集码点，批量顺序读入页缓存。
 * 翻页时调用可避免绘制过程中随机 SD seek。
 */
bool epdfont_prewarm_utf8(epdfont_t* font, const char* utf8, size_t len);

/** 清空页缓存（保留 metrics；下一次 prewarm 重建位图）。 */
void epdfont_clear_page_cache(epdfont_t* font);

#ifdef __cplusplus
}
#endif
