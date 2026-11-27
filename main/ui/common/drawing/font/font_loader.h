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
    char magic[4];         /* "EFNT" */
    uint16_t version;      /* 1 */
    uint16_t flags;        /* 保留，写 0 */
    uint32_t total_items;  /* 索引条数 */
    uint64_t index_offset; /* 索引区文件偏移 */
    uint64_t data_offset;  /* 数据区文件偏移 */
    uint32_t reserved;     /* 填 0，凑齐 32B */
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

/** @brief 关闭当前字体后端，释放文件或 Flash 映射。 */
void font_loader_deinit(void);

/** @brief 查询全局字库后端是否已初始化。 */
bool font_loader_is_ready(void);

/** @brief 获取当前字库的索引条数。 */
uint32_t font_loader_item_count(void);

/** @brief 获取全局字库后端最近一次操作的错误码。 */
int font_loader_last_error(void);

/** 组装 64-bit Key（与 Python 打包一致）。 */
static inline uint64_t fontpack_make_key(uint32_t codepoint, uint16_t size, uint16_t bpp) {
    return ((uint64_t)codepoint << 32) | ((uint64_t)size << 16) | (uint64_t)bpp;
}

/* ---------- Flash mmap（推荐长文） ---------- */

/**
 * @brief 查找并映射指定 Flash 字库分区；空标签使用默认配置。
 * @return FONT_LOADER_OK 或对应负错误码。
 */
int font_loader_init_flash(const char* partition_label);

/**
 * @brief 在 Flash 映射索引中查找字形并输出只读位图指针。
 * @note 位图在后端释放或重新初始化前有效，调用方不得释放。
 */
bool get_glyph(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphInfo* out_glyph);

/** @brief 获取当前 Flash 映射基址，未使用 Flash 后端时返回空指针。 */
const uint8_t* font_loader_mmap_base(void);

/** @brief 获取 Flash 映射字节数，未使用 Flash 后端时返回零。 */
size_t font_loader_mmap_size(void);

/* ---------- SD / FatFS ---------- */

/** @brief 打开 SD 字库并读取索引摘要，保持文件供后续查字使用。 */
int font_loader_init(const char* path);

/** @brief 在 SD 字库中查找字形，并复制位图到调用方提供的输出结构。 */
bool get_glyph_from_sd(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphData* out_glyph);

/**
 * @brief 按需打开扩展字库读取单字，返回前关闭文件，不改变全局 Flash 或 SD 后端
 * @note 调用方负责 SD 访问事务；文件不存在及缺字属于正常结果。
 * @return FONT_LOADER_OK 或对应错误码
 */
int font_loader_read_sd(const char* path, uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphData* out_glyph);

#ifdef __cplusplus
}
#endif
