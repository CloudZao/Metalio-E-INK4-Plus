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

/** @brief 打开 SD 卡上的位图字体，成功返回句柄，失败返回空指针。 */
epdfont_t* epdfont_open(const char* path);

/** @brief 关闭字体文件并释放索引、页缓存及字体句柄。 */
void epdfont_close(epdfont_t* font);

/** @brief 获取 LVGL 字体对象，其生命周期与字体句柄相同。 */
const lv_font_t* epdfont_get_lv_font(const epdfont_t* font);

/** @brief 获取字形位图的每像素位数，空句柄返回零。 */
uint8_t epdfont_get_bpp(const epdfont_t* font);

/** @brief 获取字体字形数量，空句柄返回零。 */
uint32_t epdfont_glyph_count(const epdfont_t* font);

/** @brief 按 UTF-8 文本收集码点并预读字形到页缓存，供后续绘制复用。 */
bool epdfont_prewarm_utf8(epdfont_t* font, const char* utf8, size_t len);

/** @brief 清空字形页缓存并保留度量信息，下一次预热重新载入位图。 */
void epdfont_clear_page_cache(epdfont_t* font);

#ifdef __cplusplus
}
#endif
