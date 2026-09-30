/**
 * fontpack 文件后端（模拟器 / WASM）：仅 SD 路径，无 Flash mmap。
 */

#include "font_loader.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "fontpack";

typedef enum {
    FONT_BACKEND_NONE = 0,
    FONT_BACKEND_SD = 1,
} font_backend_t;

typedef struct {
    font_backend_t backend;
    uint32_t total_items;
    uint32_t index_offset;
    uint32_t data_offset;
    int last_err;
    FILE* fp;
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
        return false;
    }
    return true;
}

static bool seek_read(FILE* fp, uint32_t off, void* dst, size_t n) {
    if (fseek(fp, (long)off, SEEK_SET) != 0) {
        return false;
    }
    return read_exact(fp, dst, n);
}

void font_loader_deinit(void) {
    if (s_ctx.fp != NULL) {
        fclose(s_ctx.fp);
        s_ctx.fp = NULL;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

bool font_loader_is_ready(void) {
    return s_ctx.backend == FONT_BACKEND_SD && s_ctx.fp != NULL;
}

uint32_t font_loader_item_count(void) {
    return font_loader_is_ready() ? s_ctx.total_items : 0;
}

int font_loader_last_error(void) {
    return s_ctx.last_err;
}

const uint8_t* font_loader_mmap_base(void) {
    return NULL;
}

size_t font_loader_mmap_size(void) {
    return 0;
}

int font_loader_init_flash(const char* partition_label) {
    (void)partition_label;
    s_ctx.last_err = FONT_LOADER_ERR_FILE;
    return FONT_LOADER_ERR_FILE;
}

bool get_glyph(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphInfo* out_glyph) {
    GlyphData tmp;
    if (!get_glyph_from_sd(codepoint, size, bpp, &tmp)) {
        return false;
    }
    if (out_glyph == NULL) {
        return false;
    }
    out_glyph->width = tmp.width;
    out_glyph->height = tmp.height;
    out_glyph->x_offset = tmp.x_offset;
    out_glyph->y_offset = tmp.y_offset;
    out_glyph->advance = tmp.advance;
    out_glyph->bmp_len = tmp.bmp_len;
    static __thread uint8_t s_bmp_cache[FONT_GLYPH_BMP_MAX];
    if (tmp.bmp_len > 0 && tmp.bmp_len <= FONT_GLYPH_BMP_MAX) {
        memcpy(s_bmp_cache, tmp.bitmap, tmp.bmp_len);
        out_glyph->bitmap = s_bmp_cache;
    } else {
        out_glyph->bitmap = NULL;
    }
    return true;
}

int font_loader_init(const char* path) {
    if (path == NULL || path[0] == '\0') {
        s_ctx.last_err = FONT_LOADER_ERR_FILE;
        return FONT_LOADER_ERR_FILE;
    }
    if (s_ctx.backend != FONT_BACKEND_NONE) {
        font_loader_deinit();
    }
    memset(&s_ctx, 0, sizeof(s_ctx));

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open failed: %s", path);
        s_ctx.last_err = FONT_LOADER_ERR_FILE;
        return FONT_LOADER_ERR_FILE;
    }

    fontpack_header_t hdr;
    if (!read_exact(fp, &hdr, sizeof(hdr)) || !header_ok(&hdr, 0)) {
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

    ESP_LOGI(TAG, "opened %s items=%lu", path, (unsigned long)s_ctx.total_items);
    return FONT_LOADER_OK;
}

bool get_glyph_from_sd(uint32_t codepoint, uint16_t size, uint16_t bpp, GlyphData* out_glyph) {
    if (out_glyph == NULL) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
        return false;
    }
    memset(out_glyph, 0, sizeof(*out_glyph));

    if (!font_loader_is_ready()) {
        s_ctx.last_err = FONT_LOADER_ERR_STATE;
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
        s_ctx.last_err = FONT_LOADER_ERR_FORMAT;
        return false;
    }

    const uint16_t bmp_len = (uint16_t)(entry.data_size - sizeof(fontpack_glyph_meta_t));
    if (bmp_len > FONT_GLYPH_BMP_MAX) {
        s_ctx.last_err = FONT_LOADER_ERR_TOO_LARGE;
        return false;
    }

    fontpack_glyph_meta_t meta;
    if (!seek_read(s_ctx.fp, entry.data_offset, &meta, sizeof(meta))) {
        s_ctx.last_err = FONT_LOADER_ERR_IO;
        return false;
    }

    if (bmp_len > 0) {
        if (fseek(s_ctx.fp, (long)(entry.data_offset + sizeof(meta)), SEEK_SET) != 0 ||
            !read_exact(s_ctx.fp, out_glyph->bitmap, bmp_len)) {
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
