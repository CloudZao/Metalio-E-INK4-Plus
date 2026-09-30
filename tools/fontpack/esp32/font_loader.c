/**
 * fonts.fontpack 检索：SD 文件二分 / Flash 分区 mmap 零拷贝。
 */

#include "font_loader.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char* TAG = "fontpack";

_Static_assert(sizeof(fontpack_header_t) == 32, "fontpack_header_t must be 32 bytes");
_Static_assert(sizeof(fontpack_index_entry_t) == 16, "fontpack_index_entry_t must be 16 bytes");
_Static_assert(sizeof(fontpack_glyph_meta_t) == 10, "fontpack_glyph_meta_t must be 10 bytes");

typedef enum {
    FONT_BACKEND_NONE = 0,
    FONT_BACKEND_SD = 1,
    FONT_BACKEND_FLASH = 2,
} font_backend_t;

typedef struct {
    font_backend_t backend;
    uint32_t total_items;
    uint32_t index_offset; /* 实际 <4GiB，存 u32 即可 */
    uint32_t data_offset;
    int last_err;

    /* SD */
    FILE* fp;

    /* Flash mmap — 映射落在 0x3C000000 等 Cache 窗口，不占 SRAM 存放字体本体 */
    const uint8_t* mmap_ptr;
    size_t mmap_size;
    esp_partition_mmap_handle_t mmap_handle;
} font_loader_ctx_t;

static font_loader_ctx_t s_ctx;

static bool header_ok(const fontpack_header_t* hdr, size_t blob_size) {
    if (hdr->magic[0] != FONTPACK_MAGIC_0 || hdr->magic[1] != FONTPACK_MAGIC_1 ||
        hdr->magic[2] != FONTPACK_MAGIC_2 || hdr->magic[3] != FONTPACK_MAGIC_3) {
        ESP_LOGE(TAG, "bad magic: %.4s", hdr->magic);
        return false;
    }
    if (hdr->version != FONTPACK_VERSION) {
        ESP_LOGE(TAG, "unsupported version %u", (unsigned)hdr->version);
        return false;
    }
    if (hdr->index_offset > 0xFFFFFFFFu || hdr->data_offset > 0xFFFFFFFFu) {
        ESP_LOGE(TAG, "offsets exceed 4GiB");
        return false;
    }
    const uint64_t index_end =
        hdr->index_offset + (uint64_t)hdr->total_items * sizeof(fontpack_index_entry_t);
    if (index_end > hdr->data_offset) {
        ESP_LOGE(TAG, "index/data overlap or corrupt header");
        return false;
    }
    if (blob_size > 0 && hdr->data_offset > blob_size) {
        ESP_LOGE(TAG, "data_offset beyond blob");
        return false;
    }
    return true;
}

static bool read_exact(FILE* fp, void* dst, size_t n) {
    size_t got = fread(dst, 1, n, fp);
    if (got != n) {
        if (ferror(fp)) {
            ESP_LOGE(TAG, "fread failed errno=%d (%s)", errno, strerror(errno));
        } else if (feof(fp)) {
            ESP_LOGE(TAG, "fread unexpected EOF (want %u got %u)", (unsigned)n, (unsigned)got);
        }
        return false;
    }
    return true;
}

static bool seek_read(FILE* fp, uint32_t off, void* dst, size_t n) {
    if (fseek(fp, (long)off, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek(%lu) failed errno=%d (%s)", (unsigned long)off, errno,
                 strerror(errno));
        return false;
    }
    return read_exact(fp, dst, n);
}

/* ======================== 公共 ======================== */

void font_loader_deinit(void) {
    if (s_ctx.fp != NULL) {
        fclose(s_ctx.fp);
        s_ctx.fp = NULL;
    }
    if (s_ctx.mmap_handle != 0) {
        esp_partition_munmap(s_ctx.mmap_handle);
        s_ctx.mmap_handle = 0;
        s_ctx.mmap_ptr = NULL;
        s_ctx.mmap_size = 0;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

bool font_loader_is_ready(void) {
    if (s_ctx.backend == FONT_BACKEND_SD) {
        return s_ctx.fp != NULL;
    }
    if (s_ctx.backend == FONT_BACKEND_FLASH) {
        return s_ctx.mmap_ptr != NULL && s_ctx.mmap_size >= sizeof(fontpack_header_t);
    }
    return false;
}

uint32_t font_loader_item_count(void) {
    return font_loader_is_ready() ? s_ctx.total_items : 0;
}

int font_loader_last_error(void) {
    return s_ctx.last_err;
}

const uint8_t* font_loader_mmap_base(void) {
    return s_ctx.backend == FONT_BACKEND_FLASH ? s_ctx.mmap_ptr : NULL;
}

size_t font_loader_mmap_size(void) {
    return s_ctx.backend == FONT_BACKEND_FLASH ? s_ctx.mmap_size : 0;
}

/* ======================== Flash mmap ======================== */

int font_loader_init_flash(const char* partition_label) {
    if (s_ctx.backend != FONT_BACKEND_NONE) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        ESP_LOGW(TAG, "already initialized; call font_loader_deinit() first");
        return FONT_LOADER_ERR_STATE;
    }

    const char* label =
        (partition_label != NULL && partition_label[0] != '\0') ? partition_label
                                                                : FONTPACK_PARTITION_LABEL;

    memset(&s_ctx, 0, sizeof(s_ctx));

    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", label);
        s_ctx.last_err = FONT_LOADER_ERR_FILE;
        return FONT_LOADER_ERR_FILE;
    }

    const void* mapped = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err =
        esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap(%s size=%lu) failed: %s", label,
                 (unsigned long)part->size, esp_err_to_name(err));
        s_ctx.last_err = FONT_LOADER_ERR_IO;
        return FONT_LOADER_ERR_IO;
    }

    const uint8_t* base = (const uint8_t*)mapped;
    if (part->size < sizeof(fontpack_header_t)) {
        esp_partition_munmap(handle);
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return FONT_LOADER_ERR_FORMAT;
    }

    fontpack_header_t hdr;
    memcpy(&hdr, base, sizeof(hdr)); /* 避免可能的未对齐问题 */
    if (!header_ok(&hdr, part->size)) {
        esp_partition_munmap(handle);
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return FONT_LOADER_ERR_FORMAT;
    }

    s_ctx.backend = FONT_BACKEND_FLASH;
    s_ctx.mmap_ptr = base;
    s_ctx.mmap_size = part->size;
    s_ctx.mmap_handle = handle;
    s_ctx.total_items = hdr.total_items;
    s_ctx.index_offset = (uint32_t)hdr.index_offset;
    s_ctx.data_offset = (uint32_t)hdr.data_offset;
    s_ctx.last_err = FONT_LOADER_OK;

    ESP_LOGI(TAG,
             "flash mmap '%s' ptr=%p size=%u items=%lu index@%lu data@%lu "
             "(ESP32-S3 data window ~0x3C000000)",
             label, (void*)base, (unsigned)part->size, (unsigned long)s_ctx.total_items,
             (unsigned long)s_ctx.index_offset, (unsigned long)s_ctx.data_offset);
    return FONT_LOADER_OK;
}

bool get_glyph(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphInfo* out_glyph) {
    if (out_glyph == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        return false;
    }
    memset(out_glyph, 0, sizeof(*out_glyph));

    if (s_ctx.backend != FONT_BACKEND_FLASH || s_ctx.mmap_ptr == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        ESP_LOGE(TAG, "get_glyph requires font_loader_init_flash()");
        return false;
    }
    if (s_ctx.total_items == 0) {
        s_ctx.last_err = FONT_LOADER_ERR_NOT_FOUND;
        return false;
    }

    const uint8_t* base = s_ctx.mmap_ptr;
    const fontpack_index_entry_t* index =
        (const fontpack_index_entry_t*)(base + s_ctx.index_offset);
    const uint64_t target = fontpack_make_key(codepoint, size, bpp);

    int32_t lo = 0;
    int32_t hi = (int32_t)s_ctx.total_items - 1;
    const fontpack_index_entry_t* hit = NULL;

    /* 纯内存二分：索引在 Flash Cache 窗口，无 SRAM 拷贝、无 fseek */
    while (lo <= hi) {
        const int32_t mid = lo + ((hi - lo) >> 1);
        const fontpack_index_entry_t* e = &index[mid];
        uint64_t key;
        memcpy(&key, &e->key, sizeof(key)); /* 保证对齐安全 */

        if (key < target) {
            lo = mid + 1;
        } else if (key > target) {
            hi = mid - 1;
        } else {
            hit = e;
            break;
        }
    }

    if (hit == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_NOT_FOUND;
        return false;
    }

    uint32_t data_off;
    uint16_t data_size;
    memcpy(&data_off, &hit->data_offset, sizeof(data_off));
    memcpy(&data_size, &hit->data_size, sizeof(data_size));

    if (data_size < sizeof(fontpack_glyph_meta_t)) {
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return false;
    }
    if ((uint64_t)data_off + data_size > s_ctx.mmap_size) {
        ESP_LOGE(TAG, "glyph out of mapped range off=%lu size=%u", (unsigned long)data_off,
                 (unsigned)data_size);
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return false;
    }

    const uint8_t* glyph_ptr = base + data_off;
    fontpack_glyph_meta_t meta;
    memcpy(&meta, glyph_ptr, sizeof(meta));

    const uint16_t bmp_len = (uint16_t)(data_size - sizeof(fontpack_glyph_meta_t));
    out_glyph->width = meta.width;
    out_glyph->height = meta.height;
    out_glyph->x_offset = meta.x_offset;
    out_glyph->y_offset = meta.y_offset;
    out_glyph->advance = meta.advance;
    out_glyph->bmp_len = bmp_len;
    /* 零拷贝：直接返回 Flash mmap 内位图地址 */
    out_glyph->bitmap = (bmp_len > 0) ? (glyph_ptr + sizeof(fontpack_glyph_meta_t)) : NULL;

    s_ctx.last_err = FONT_LOADER_OK;
    return true;
}

/* ======================== SD / FatFS ======================== */

int font_loader_init(const char* path) {
    if (path == NULL || path[0] == '\0') {
        s_ctx.last_err = FONT_LOADER_ERR_FILE;
        return FONT_LOADER_ERR_FILE;
    }
    if (s_ctx.backend != FONT_BACKEND_NONE) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        ESP_LOGW(TAG, "already initialized; call font_loader_deinit() first");
        return FONT_LOADER_ERR_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open failed: %s errno=%d (%s)", path, errno, strerror(errno));
        s_ctx.last_err = FONT_LOADER_ERR_FILE;
        return FONT_LOADER_ERR_FILE;
    }

    fontpack_header_t hdr;
    if (!read_exact(fp, &hdr, sizeof(hdr))) {
        fclose(fp);
        s_ctx.last_err = FONT_LOADER_ERR_IO;
        return FONT_LOADER_ERR_IO;
    }
    if (!header_ok(&hdr, 0)) {
        fclose(fp);
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return FONT_LOADER_ERR_FORMAT;
    }

    s_ctx.backend = FONT_BACKEND_SD;
    s_ctx.fp = fp;
    s_ctx.total_items = hdr.total_items;
    s_ctx.index_offset = (uint32_t)hdr.index_offset;
    s_ctx.data_offset = (uint32_t)hdr.data_offset;
    s_ctx.last_err = FONT_LOADER_OK;

    ESP_LOGI(TAG, "opened %s items=%lu index@%lu data@%lu", path,
             (unsigned long)s_ctx.total_items, (unsigned long)s_ctx.index_offset,
             (unsigned long)s_ctx.data_offset);
    return FONT_LOADER_OK;
}

bool get_glyph_from_sd(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphData* out_glyph) {
    if (out_glyph == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        return false;
    }
    memset(out_glyph, 0, sizeof(*out_glyph));

    if (s_ctx.backend != FONT_BACKEND_SD || s_ctx.fp == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        ESP_LOGE(TAG, "get_glyph_from_sd requires font_loader_init()");
        return false;
    }
    if (s_ctx.total_items == 0) {
        s_ctx.last_err = FONT_LOADER_ERR_NOT_FOUND;
        return false;
    }

    const uint64_t target = fontpack_make_key(codepoint, size, bpp);
    int32_t lo = 0;
    int32_t hi = (int32_t)s_ctx.total_items - 1;
    fontpack_index_entry_t entry;
    bool found = false;

    while (lo <= hi) {
        const int32_t mid = lo + ((hi - lo) >> 1);
        const uint32_t off =
            s_ctx.index_offset + (uint32_t)mid * (uint32_t)sizeof(fontpack_index_entry_t);

        if (!seek_read(s_ctx.fp, off, &entry, sizeof(entry))) {
            s_ctx.last_err = FONT_LOADER_ERR_IO;
            return false;
        }

        if (entry.key < target) {
            lo = mid + 1;
        } else if (entry.key > target) {
            hi = mid - 1;
        } else {
            found = true;
            break;
        }
    }

    if (!found) {
        s_ctx.last_err = FONT_LOADER_ERR_NOT_FOUND;
        return false;
    }

    if (entry.data_size < sizeof(fontpack_glyph_meta_t)) {
        ESP_LOGE(TAG, "corrupt data_size=%u", (unsigned)entry.data_size);
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return false;
    }

    const uint16_t bmp_len = (uint16_t)(entry.data_size - sizeof(fontpack_glyph_meta_t));
    if (bmp_len > FONT_GLYPH_BMP_MAX) {
        ESP_LOGE(TAG, "glyph bmp %u > FONT_GLYPH_BMP_MAX %u (U+%04lX size=%u bpp=%u)",
                 (unsigned)bmp_len, (unsigned)FONT_GLYPH_BMP_MAX, (unsigned long)codepoint,
                 (unsigned)size, (unsigned)bpp);
        s_ctx.last_err = FONT_LOADER_ERR_TOO_LARGE;
        return false;
    }

    fontpack_glyph_meta_t meta;
    if (!seek_read(s_ctx.fp, entry.data_offset, &meta, sizeof(meta))) {
        s_ctx.last_err = FONT_LOADER_ERR_IO;
        return false;
    }

    if (bmp_len > 0) {
        if (!read_exact(s_ctx.fp, out_glyph->bitmap, bmp_len)) {
            s_ctx.last_err = FONT_LOADER_ERR_IO;
            return false;
        }
    }

    out_glyph->width = meta.width;
    out_glyph->height = meta.height;
    out_glyph->x_offset = meta.x_offset;
    out_glyph->y_offset = meta.y_offset;
    out_glyph->advance = meta.advance;
    out_glyph->bmp_len = bmp_len;

    s_ctx.last_err = FONT_LOADER_OK;
    return true;
}
