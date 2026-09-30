/**
 * @file epd_font24.c
 * @brief font_data 分区 mmap + ebook 点阵绘制
 */

#include "epd_font24.h"

#include "epdiy.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <string.h>

#define TAG "epd_font24"

#define EPD_FONT24_MAGIC "EPD24FNT"
#define EPD_FONT24_HEADER_SIZE 32
#define EPD_FONT24_GLYPH_BYTES 72
#define EPD_FONT24_RECORD_SIZE (2 + EPD_FONT24_GLYPH_BYTES) // 74

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint16_t char_size;
    uint16_t glyph_bytes;
    uint32_t num_chars;
    uint8_t reserved[12];
} epd_font24_header_t;

_Static_assert(sizeof(epd_font24_header_t) == EPD_FONT24_HEADER_SIZE, "header size");

static const esp_partition_t* s_part = NULL;
static const uint8_t* s_map = NULL;
static esp_partition_mmap_handle_t s_mmap = 0;
static uint32_t s_num_chars = 0;
static bool s_ready = false;

static const uint8_t* glyph_record(uint32_t index) {
    return s_map + EPD_FONT24_HEADER_SIZE + index * EPD_FONT24_RECORD_SIZE;
}

static const uint8_t* find_glyph_bitmap(uint16_t unicode) {
    if (!s_ready || s_map == NULL) {
        return NULL;
    }
    int left = 0;
    int right = (int)s_num_chars - 1;
    while (left <= right) {
        const int mid = left + (right - left) / 2;
        const uint8_t* rec = glyph_record((uint32_t)mid);
        const uint16_t u = (uint16_t)rec[0] | ((uint16_t)rec[1] << 8);
        if (u == unicode) {
            return rec + 2;
        }
        if (u < unicode) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return NULL;
}

bool epd_font24_init(void) {
    if (s_ready) {
        return true;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      EPD_FONT24_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", EPD_FONT24_PARTITION_LABEL);
        return false;
    }

    const void* ptr = NULL;
    esp_err_t err = esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &ptr, &s_mmap);
    if (err != ESP_OK || ptr == NULL) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return false;
    }
    s_map = (const uint8_t*)ptr;

    const epd_font24_header_t* hdr = (const epd_font24_header_t*)s_map;
    if (memcmp(hdr->magic, EPD_FONT24_MAGIC, 8) != 0 || hdr->version != 1 ||
        hdr->char_size != EPD_FONT24_SIZE || hdr->glyph_bytes != EPD_FONT24_GLYPH_BYTES ||
        hdr->num_chars == 0) {
        ESP_LOGE(TAG, "bad font header magic/ver/size (flash epd_font24.bin to font_data?)");
        esp_partition_munmap(s_mmap);
        s_map = NULL;
        s_mmap = 0;
        return false;
    }

    const size_t need = EPD_FONT24_HEADER_SIZE + (size_t)hdr->num_chars * EPD_FONT24_RECORD_SIZE;
    if (need > s_part->size) {
        ESP_LOGE(TAG, "font larger than partition (%u > %u)", (unsigned)need, (unsigned)s_part->size);
        esp_partition_munmap(s_mmap);
        s_map = NULL;
        s_mmap = 0;
        return false;
    }

    s_num_chars = hdr->num_chars;
    s_ready = true;
    ESP_LOGI(TAG, "ok partition=%s glyphs=%u size=%u", EPD_FONT24_PARTITION_LABEL,
             (unsigned)s_num_chars, (unsigned)need);
    return true;
}

bool epd_font24_ready(void) {
    return s_ready;
}

void epd_font24_draw_char(uint8_t* fb, int x, int y, uint16_t unicode, uint8_t color) {
    epd_font24_draw_char_scaled(fb, x, y, unicode, color, 1);
}

static void fill_logical_rect(uint8_t* fb, int lx, int ly, int lw, int lh, uint8_t color) {
    if (fb == NULL || lw <= 0 || lh <= 0) {
        return;
    }
    EpdRect r = {
        .x = ly,
        .y = EPD_LOGICAL_W - lx - lw,
        .width = lh,
        .height = lw,
    };
    epd_fill_rect(r, color, fb);
}

void epd_font24_draw_char_scaled(uint8_t* fb, int x, int y, uint16_t unicode, uint8_t color,
                                 int scale) {
    if (fb == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    const uint8_t* bitmap = find_glyph_bitmap(unicode);
    if (bitmap == NULL) {
        ESP_LOGW(TAG, "missing U+%04X scale=%d → placeholder", (unsigned)unicode, scale);
        if (scale == 1) {
            epd_draw_rect((EpdRect){y, EPD_LOGICAL_W - 1 - x - EPD_FONT24_SIZE, EPD_FONT24_SIZE,
                                    EPD_FONT24_SIZE},
                          color, fb);
        } else {
            fill_logical_rect(fb, x, y, EPD_FONT24_SIZE * scale, EPD_FONT24_SIZE * scale, color);
        }
        return;
    }

    int bits = 0;
    for (int row = 0; row < EPD_FONT24_SIZE; ++row) {
        for (int col = 0; col < EPD_FONT24_SIZE; ++col) {
            const int byte_idx = row * 3 + (col / 8);
            const int bit_idx = 7 - (col % 8);
            if (((bitmap[byte_idx] >> bit_idx) & 1) == 0) {
                continue;
            }
            bits += 1;
            if (scale == 1) {
                int px = 0;
                int py = 0;
                epd_logical_to_physical(x + col, y + row, &px, &py);
                epd_draw_pixel(px, py, color, fb);
            } else {
                fill_logical_rect(fb, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }

    // 每种 unicode 只打一次，避免刷屏
    static uint16_t s_logged[8];
    static int s_nlog = 0;
    bool seen = false;
    for (int i = 0; i < s_nlog; ++i) {
        if (s_logged[i] == unicode) {
            seen = true;
            break;
        }
    }
    if (!seen && s_nlog < 8) {
        s_logged[s_nlog++] = unicode;
        ESP_LOGI(TAG, "path glyph U+%04X bits=%d scale=%d plot=%s @(%d,%d)", (unsigned)unicode,
                 bits, scale, scale == 1 ? "draw_pixel" : "fill_rect", x, y);
    }
}

static uint16_t decode_utf8(const char** str) {
    const uint8_t c = (uint8_t)**str;
    if (c == 0) {
        return 0;
    }
    (*str)++;
    if (c < 0x80) {
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        const uint8_t c2 = (uint8_t)**str;
        if (c2 == 0) {
            return c;
        }
        (*str)++;
        return (uint16_t)(((c & 0x1F) << 6) | (c2 & 0x3F));
    }
    if ((c & 0xF0) == 0xE0) {
        const uint8_t c2 = (uint8_t)**str;
        if (c2 == 0) {
            return c;
        }
        (*str)++;
        const uint8_t c3 = (uint8_t)**str;
        if (c3 == 0) {
            return c;
        }
        (*str)++;
        return (uint16_t)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
    }
    return (uint16_t)'?';
}

static int char_advance(uint16_t unicode) {
    return (unicode < 128) ? EPD_FONT24_ASCII_ADVANCE : EPD_FONT24_SIZE;
}

int epd_font24_draw_string(uint8_t* fb, int x, int y, const char* text, uint8_t color) {
    return epd_font24_draw_string_scaled(fb, x, y, text, color, 1);
}

int epd_font24_draw_string_scaled(uint8_t* fb, int x, int y, const char* text, uint8_t color,
                                  int scale) {
    if (fb == NULL || text == NULL) {
        return x;
    }
    if (scale < 1) {
        scale = 1;
    }
    int cx = x;
    const char* p = text;
    while (*p != '\0') {
        const uint16_t u = decode_utf8(&p);
        if (u == 0) {
            break;
        }
        epd_font24_draw_char_scaled(fb, cx, y, u, color, scale);
        cx += char_advance(u) * scale;
    }
    return cx;
}

static int measure_string_width(const char* text) {
    int w = 0;
    const char* p = text;
    while (*p != '\0') {
        const uint16_t u = decode_utf8(&p);
        if (u == 0) {
            break;
        }
        w += char_advance(u);
    }
    return w;
}

void epd_font24_draw_string_centered(uint8_t* fb, int cx, int y, const char* text, uint8_t color,
                                     int fb_w) {
    epd_font24_draw_string_centered_scaled(fb, cx, y, text, color, fb_w, 1);
}

void epd_font24_draw_string_centered_scaled(uint8_t* fb, int cx, int y, const char* text,
                                            uint8_t color, int fb_w, int scale) {
    if (text == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    const int w = measure_string_width(text) * scale;
    int x = cx - w / 2;
    if (x < 0) {
        x = 0;
    }
    if (fb_w > 0 && x + w > fb_w) {
        x = fb_w - w;
        if (x < 0) {
            x = 0;
        }
    }
    epd_font24_draw_string_scaled(fb, x, y, text, color, scale);
}
