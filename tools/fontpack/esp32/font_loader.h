#pragma once

/**
 * fonts.fontpack 检索库
 *
 * 两种后端：
 *  1) SD / FatFS：font_loader_init() + get_glyph_from_sd()
 *     文件二分 + 单字拷贝到 GlyphData（栈 Buffer）
 *  2) Flash 分区 mmap：font_loader_init_flash() + get_glyph()
 *     将 font_data 分区映射到 CPU 虚拟地址（ESP32-S3 上多为 0x3C000000 段）
 *     索引/位图零拷贝，字形数据不占物理 SRAM
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONTPACK_MAGIC_0 'E'
#define FONTPACK_MAGIC_1 'F'
#define FONTPACK_MAGIC_2 'N'
#define FONTPACK_MAGIC_3 'T'
#define FONTPACK_VERSION 1

/** 默认 Flash 分区 label（与分区表 Name 字段一致，如 font_data）。 */
#ifndef FONTPACK_PARTITION_LABEL
#define FONTPACK_PARTITION_LABEL "font_data"
#endif

/** SD 路径下单字位图最大字节数（仅 get_glyph_from_sd 使用）。 */
#ifndef FONT_GLYPH_BMP_MAX
#define FONT_GLYPH_BMP_MAX 1024
#endif

#pragma pack(push, 1)

/** 文件头：恰好 32 字节。 */
typedef struct {
    char magic[4];          /* "EFNT" */
    uint16_t version;       /* 1 */
    uint16_t flags;         /* 保留，写 0 */
    uint32_t total_items;   /* 索引条数 */
    uint64_t index_offset;  /* 索引区文件偏移 */
    uint64_t data_offset;   /* 数据区文件偏移 */
    uint32_t reserved;      /* 填 0，凑齐 32B */
} fontpack_header_t;

/** 索引条目：恰好 16 字节。Key = (unicode<<32)|(size<<16)|bpp */
typedef struct {
    uint64_t key;
    uint32_t data_offset; /* 相对文件头的绝对偏移 */
    uint16_t data_size;   /* 元数据 + 位图总长 */
    uint16_t reserved;
} fontpack_index_entry_t;

/** 磁盘上的字形元数据：恰好 10 字节，后接 bitmap。 */
typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t x_offset;
    int16_t y_offset;
    uint16_t advance;
} fontpack_glyph_meta_t;

#pragma pack(pop)

/**
 * Flash mmap 零拷贝输出：bitmap 指向映射区内的只读 Flash 窗口。
 * 指针在 font_loader_deinit() / 重新 init 前有效。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t x_offset;
    int16_t y_offset;
    uint16_t advance;
    uint16_t bmp_len;
    const uint8_t* bitmap; /* 零拷贝：指向 mmap 虚拟地址，勿 free */
} GlyphInfo;

/**
 * SD 路径拷贝输出：位图写入调用方 Buffer。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t x_offset;
    int16_t y_offset;
    uint16_t advance;
    uint16_t bmp_len;
    uint8_t bitmap[FONT_GLYPH_BMP_MAX];
} GlyphData;

typedef enum {
    FONT_LOADER_OK = 0,
    FONT_LOADER_ERR_FILE = -1,      /* 打开/分区不存在 */
    FONT_LOADER_ERR_FORMAT = -2,    /* Magic/版本/尺寸不对 */
    FONT_LOADER_ERR_NOT_FOUND = -3, /* Key 不在索引中 */
    FONT_LOADER_ERR_TOO_LARGE = -4, /* 单字超过 FONT_GLYPH_BMP_MAX（仅 SD） */
    FONT_LOADER_ERR_IO = -5,        /* fseek/fread / mmap 失败 */
    FONT_LOADER_ERR_STATE = -6,     /* 未初始化 / 重复 init */
} font_loader_err_t;

/* ---------- 公共 ---------- */

void font_loader_deinit(void);
bool font_loader_is_ready(void);
uint32_t font_loader_item_count(void);
int font_loader_last_error(void);

/** 组装 64-bit Key（与 Python 打包一致）。 */
static inline uint64_t fontpack_make_key(uint32_t codepoint, uint16_t size, uint16_t bpp) {
    return ((uint64_t)codepoint << 32) | ((uint64_t)size << 16) | (uint64_t)bpp;
}

/* ---------- Flash mmap（推荐长文） ---------- */

/**
 * 查找并 mmap 名为 label 的分区（默认 FONTPACK_PARTITION_LABEL="font_data"）。
 * 映射后指针落在 Flash Cache 窗口（ESP32-S3 数据段常见为 0x3Cxxxxxx）。
 * 字体本体不占用物理 SRAM；仅保留句柄与 Header 摘要。
 *
 * @param partition_label  NULL 则用 "font_data"
 * @return FONT_LOADER_OK 或负错误码
 */
int font_loader_init_flash(const char* partition_label);

/**
 * O(log N) 内存二分（直接访问 mmap 索引）+ 零拷贝解析字形。
 * out_glyph->bitmap 指向 Flash 映射区，绘制时直接读即可。
 */
bool get_glyph(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphInfo* out_glyph);

/** 当前 mmap 基址（未 flash-init 时为 NULL）。调试用。 */
const uint8_t* font_loader_mmap_base(void);

/** 映射字节数。 */
size_t font_loader_mmap_size(void);

/* ---------- SD / FatFS ---------- */

/**
 * 打开路径（如 /sdcard/metalio/e-ink/fonts/fonts.fontpack），读 Header，保持 FILE*。
 */
int font_loader_init(const char* path);

/**
 * 文件二分查找，单字拷贝到 out_glyph->bitmap。
 */
bool get_glyph_from_sd(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphData* out_glyph);

#ifdef __cplusplus
}
#endif
