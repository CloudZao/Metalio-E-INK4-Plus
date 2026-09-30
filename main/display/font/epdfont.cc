#include "epdfont.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

static_assert(sizeof(epdfont_header_t) == 64, "epdfont_header_t size");
static_assert(sizeof(epdfont_interval_t) == 12, "epdfont_interval_t size");
static_assert(sizeof(epdfont_glyph_t) == 16, "epdfont_glyph_t size");

namespace {

constexpr const char* TAG = "epdfont";
constexpr size_t kHeaderSize = sizeof(epdfont_header_t);
constexpr size_t kMaxPageGlyphs = 512;
constexpr size_t kOverflowSlots = 16;
constexpr size_t kMaxBmpPerGlyph = 2048;
constexpr size_t kPageBitmapBudget = 96 * 1024;

static const uint8_t kOpa2Table[4] = {0, 85, 170, 255};

struct PageSlot {
    uint32_t glyph_index = UINT32_MAX;
    uint16_t bmp_len = 0;
    uint32_t buf_off = 0;  // into page_bitmap_buf_
};

struct OverflowSlot {
    uint32_t glyph_index = UINT32_MAX;
    uint16_t bmp_len = 0;
    uint8_t* data = nullptr;
};

}  // namespace

struct epdfont_t {
    FILE* fp = nullptr;
    epdfont_header_t hdr{};
    uint8_t bpp = 2;

    epdfont_interval_t* intervals = nullptr;
    epdfont_glyph_t* glyphs = nullptr;

    // 页级位图缓存（连续缓冲 + 索引）
    uint8_t* page_bitmap_buf = nullptr;
    size_t page_bitmap_cap = 0;
    size_t page_bitmap_used = 0;
    PageSlot* page_slots = nullptr;
    size_t page_slot_count = 0;

    // 漏字 overflow 环缓冲
    OverflowSlot overflow[kOverflowSlots]{};
    size_t overflow_next = 0;

    lv_font_t lv{};
};

static bool ReadExact(FILE* fp, void* dst, size_t n) {
    return std::fread(dst, 1, n, fp) == n;
}

static bool SeekRead(FILE* fp, uint32_t off, void* dst, size_t n) {
    if (std::fseek(fp, static_cast<long>(off), SEEK_SET) != 0) {
        return false;
    }
    return ReadExact(fp, dst, n);
}

static int FindInterval(const epdfont_t* f, uint32_t cp) {
    if (f == nullptr || f->intervals == nullptr || f->hdr.interval_count == 0) {
        return -1;
    }
    int lo = 0;
    int hi = static_cast<int>(f->hdr.interval_count) - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const epdfont_interval_t& iv = f->intervals[mid];
        if (cp < iv.first) {
            hi = mid - 1;
        } else if (cp > iv.last) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

static bool LookupGlyphIndex(const epdfont_t* f, uint32_t cp, uint32_t* out_gi) {
    if (f == nullptr || out_gi == nullptr) {
        return false;
    }
    const int iv = FindInterval(f, cp);
    if (iv < 0) {
        return false;
    }
    const uint32_t gi = f->intervals[iv].glyph_index + (cp - f->intervals[iv].first);
    if (gi >= f->hdr.glyph_count) {
        return false;
    }
    *out_gi = gi;
    return true;
}

static const uint8_t* FindCachedBitmap(epdfont_t* f, uint32_t gi, uint16_t* out_len) {
    for (size_t i = 0; i < f->page_slot_count; ++i) {
        if (f->page_slots[i].glyph_index == gi) {
            if (out_len) {
                *out_len = f->page_slots[i].bmp_len;
            }
            return f->page_bitmap_buf + f->page_slots[i].buf_off;
        }
    }
    for (size_t i = 0; i < kOverflowSlots; ++i) {
        if (f->overflow[i].glyph_index == gi && f->overflow[i].data != nullptr) {
            if (out_len) {
                *out_len = f->overflow[i].bmp_len;
            }
            return f->overflow[i].data;
        }
    }
    return nullptr;
}

static const uint8_t* LoadOverflowBitmap(epdfont_t* f, uint32_t gi) {
    const epdfont_glyph_t& g = f->glyphs[gi];
    if (g.bmp_len == 0 || g.bmp_len > kMaxBmpPerGlyph) {
        return nullptr;
    }
    OverflowSlot& slot = f->overflow[f->overflow_next % kOverflowSlots];
    f->overflow_next++;
    if (slot.data != nullptr) {
        heap_caps_free(slot.data);
        slot.data = nullptr;
    }
    slot.data = static_cast<uint8_t*>(
        heap_caps_malloc(g.bmp_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slot.data == nullptr) {
        slot.data = static_cast<uint8_t*>(std::malloc(g.bmp_len));
    }
    if (slot.data == nullptr) {
        slot.glyph_index = UINT32_MAX;
        return nullptr;
    }
    const uint32_t file_off = f->hdr.bitmaps_offset + g.bmp_off;
    if (!SeekRead(f->fp, file_off, slot.data, g.bmp_len)) {
        heap_caps_free(slot.data);
        slot.data = nullptr;
        slot.glyph_index = UINT32_MAX;
        return nullptr;
    }
    slot.glyph_index = gi;
    slot.bmp_len = g.bmp_len;
    return slot.data;
}

static const uint8_t* EnsureBitmap(epdfont_t* f, uint32_t gi) {
    uint16_t len = 0;
    const uint8_t* p = FindCachedBitmap(f, gi, &len);
    if (p != nullptr) {
        return p;
    }
    return LoadOverflowBitmap(f, gi);
}

static void UnpackToA8(const uint8_t* src, uint8_t bpp, uint16_t box_w, uint16_t box_h, uint8_t* dst,
                       uint32_t stride_out) {
    if (bpp == 1) {
        for (uint16_t y = 0; y < box_h; ++y) {
            uint8_t* row = dst + static_cast<size_t>(y) * stride_out;
            for (uint16_t x = 0; x < box_w; ++x) {
                const uint32_t bit_i = static_cast<uint32_t>(y) * box_w + x;
                const uint8_t byte = src[bit_i >> 3];
                const uint8_t bit = 7 - (bit_i & 7);
                row[x] = (byte >> bit) & 1 ? 0xff : 0x00;
            }
        }
        return;
    }
    // bpp == 2, packed MSB-first, 4 pixels/byte, no row padding (LVGL plain)
    uint32_t bit_i = 0;
    for (uint16_t y = 0; y < box_h; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * stride_out;
        for (uint16_t x = 0; x < box_w; ++x) {
            const uint8_t byte = src[bit_i >> 2];
            const uint8_t shift = static_cast<uint8_t>(6 - 2 * (bit_i & 3));
            row[x] = kOpa2Table[(byte >> shift) & 0x3];
            ++bit_i;
        }
    }
}

static bool GetGlyphDscCb(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t letter,
                          uint32_t /*letter_next*/) {
    auto* f = static_cast<epdfont_t*>(const_cast<void*>(font->dsc));
    if (f == nullptr || dsc_out == nullptr) {
        return false;
    }
    if (letter == '\t') {
        letter = ' ';
    }
    uint32_t gi = 0;
    if (!LookupGlyphIndex(f, letter, &gi)) {
        return false;
    }
    const epdfont_glyph_t& g = f->glyphs[gi];
    dsc_out->adv_w = g.adv_w;
    if (letter == '\t') {
        dsc_out->adv_w = static_cast<uint16_t>(dsc_out->adv_w * 2);
    }
    dsc_out->box_w = g.width;
    dsc_out->box_h = g.height;
    dsc_out->ofs_x = g.ofs_x;
    dsc_out->ofs_y = g.ofs_y;
    dsc_out->stride = 0;
    dsc_out->format = (f->bpp == 1) ? LV_FONT_GLYPH_FORMAT_A1 : LV_FONT_GLYPH_FORMAT_A2;
    dsc_out->is_placeholder = 0;
    dsc_out->gid.index = gi + 1;  // 0 reserved as "none"
    return true;
}

static const void* GetGlyphBitmapCb(lv_font_glyph_dsc_t* g_dsc, lv_draw_buf_t* draw_buf) {
    if (g_dsc == nullptr || g_dsc->resolved_font == nullptr) {
        return nullptr;
    }
    auto* f = static_cast<epdfont_t*>(const_cast<void*>(g_dsc->resolved_font->dsc));
    if (f == nullptr || g_dsc->gid.index == 0) {
        return nullptr;
    }
    const uint32_t gi = g_dsc->gid.index - 1;
    if (gi >= f->hdr.glyph_count) {
        return nullptr;
    }
    const epdfont_glyph_t& g = f->glyphs[gi];
    if (g.width == 0 || g.height == 0 || g.bmp_len == 0) {
        return nullptr;
    }

    const uint8_t* raw = EnsureBitmap(f, gi);
    if (raw == nullptr) {
        return nullptr;
    }

    if (g_dsc->req_raw_bitmap) {
        return raw;
    }

    if (draw_buf == nullptr || draw_buf->data == nullptr) {
        return nullptr;
    }
    // 必须用调用方 draw_buf 的 stride（page_text_raster 等用大 scratch）；
    // 若自算 width stride，读侧仍按 header.stride 会错位 → 实心「豆腐块」
    uint32_t stride_out = draw_buf->header.stride;
    if (stride_out == 0) {
        stride_out = lv_draw_buf_width_to_stride(g.width, LV_COLOR_FORMAT_A8);
    }
    const size_t need = static_cast<size_t>(stride_out) * g.height;
    if (need > draw_buf->data_size) {
        ESP_LOGW(TAG, "glyph A8 buf too small gi=%u %ux%u stride=%u need=%u have=%u",
                 static_cast<unsigned>(gi), g.width, g.height, static_cast<unsigned>(stride_out),
                 static_cast<unsigned>(need), static_cast<unsigned>(draw_buf->data_size));
        return nullptr;
    }
    std::memset(draw_buf->data, 0, need);
    UnpackToA8(raw, f->bpp, g.width, g.height, draw_buf->data, stride_out);
    draw_buf->header.w = g.width;
    draw_buf->header.h = g.height;
    draw_buf->header.cf = LV_COLOR_FORMAT_A8;
    draw_buf->header.stride = stride_out;
    lv_draw_buf_flush_cache(draw_buf, nullptr);
    return draw_buf;
}

static void Utf8CollectCodepoints(const char* utf8, size_t len, std::vector<uint32_t>& out) {
    out.clear();
    out.reserve(256);
    size_t i = 0;
    while (i < len) {
        const uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = (c & 0x1F) << 6 | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = (c & 0x0F) << 12 | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6 |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
            n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            cp = (c & 0x07) << 18 | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 12 |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3F) << 6 |
                 (static_cast<uint8_t>(utf8[i + 3]) & 0x3F);
            n = 4;
        } else {
            ++i;
            continue;
        }
        i += n;
        if (cp == '\t') {
            cp = ' ';
        }
        if (cp < 0x20 && cp != '\n') {
            continue;
        }
        out.push_back(cp);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

epdfont_t* epdfont_open(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "open fail: %s", path);
        return nullptr;
    }

    epdfont_header_t hdr{};
    if (!ReadExact(fp, &hdr, kHeaderSize)) {
        ESP_LOGW(TAG, "header read fail");
        std::fclose(fp);
        return nullptr;
    }
    if (std::memcmp(hdr.magic, "EPDFONT", 7) != 0 || hdr.version != EPDFONT_VERSION) {
        ESP_LOGW(TAG, "bad magic/version");
        std::fclose(fp);
        return nullptr;
    }
    const uint8_t bpp = static_cast<uint8_t>(hdr.flags & EPDFONT_FLAG_BPP_MASK);
    if ((bpp != 1 && bpp != 2) || hdr.glyph_count == 0 || hdr.interval_count == 0) {
        ESP_LOGW(TAG, "bad header fields bpp=%u glyphs=%u intervals=%u", bpp,
                 static_cast<unsigned>(hdr.glyph_count), static_cast<unsigned>(hdr.interval_count));
        std::fclose(fp);
        return nullptr;
    }
    if (hdr.glyph_count > 200000 || hdr.interval_count > 4096) {
        ESP_LOGW(TAG, "counts too large");
        std::fclose(fp);
        return nullptr;
    }

    auto* f = new (std::nothrow) epdfont_t();
    if (f == nullptr) {
        std::fclose(fp);
        return nullptr;
    }
    f->fp = fp;
    f->hdr = hdr;
    f->bpp = bpp;

    const size_t iv_bytes = sizeof(epdfont_interval_t) * hdr.interval_count;
    const size_t gl_bytes = sizeof(epdfont_glyph_t) * hdr.glyph_count;
    f->intervals = static_cast<epdfont_interval_t*>(
        heap_caps_malloc(iv_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    f->glyphs = static_cast<epdfont_glyph_t*>(
        heap_caps_malloc(gl_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (f->intervals == nullptr) {
        f->intervals = static_cast<epdfont_interval_t*>(std::malloc(iv_bytes));
    }
    if (f->glyphs == nullptr) {
        f->glyphs = static_cast<epdfont_glyph_t*>(std::malloc(gl_bytes));
    }
    if (f->intervals == nullptr || f->glyphs == nullptr) {
        ESP_LOGE(TAG, "alloc intervals/glyphs fail");
        epdfont_close(f);
        return nullptr;
    }
    if (!SeekRead(fp, hdr.intervals_offset, f->intervals, iv_bytes) ||
        !SeekRead(fp, hdr.glyphs_offset, f->glyphs, gl_bytes)) {
        ESP_LOGE(TAG, "read tables fail");
        epdfont_close(f);
        return nullptr;
    }

    f->page_bitmap_cap = kPageBitmapBudget;
    f->page_bitmap_buf = static_cast<uint8_t*>(
        heap_caps_malloc(f->page_bitmap_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (f->page_bitmap_buf == nullptr) {
        f->page_bitmap_buf = static_cast<uint8_t*>(std::malloc(f->page_bitmap_cap));
    }
    f->page_slots = static_cast<PageSlot*>(
        heap_caps_malloc(sizeof(PageSlot) * kMaxPageGlyphs, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (f->page_slots == nullptr) {
        f->page_slots = static_cast<PageSlot*>(std::malloc(sizeof(PageSlot) * kMaxPageGlyphs));
    }
    if (f->page_bitmap_buf == nullptr || f->page_slots == nullptr) {
        ESP_LOGE(TAG, "alloc page cache fail");
        epdfont_close(f);
        return nullptr;
    }
    f->page_bitmap_used = 0;
    f->page_slot_count = 0;

    std::memset(&f->lv, 0, sizeof(f->lv));
    f->lv.get_glyph_dsc = GetGlyphDscCb;
    f->lv.get_glyph_bitmap = GetGlyphBitmapCb;
    f->lv.release_glyph = nullptr;
    f->lv.line_height = hdr.line_height;
    f->lv.base_line = hdr.base_line;
    f->lv.subpx = LV_FONT_SUBPX_NONE;
    f->lv.kerning = LV_FONT_KERNING_NONE;
    f->lv.static_bitmap = 0;
    f->lv.underline_position = -1;
    f->lv.underline_thickness = 1;
    f->lv.dsc = f;
    f->lv.fallback = nullptr;
    f->lv.user_data = nullptr;

    ESP_LOGI(TAG, "opened %s: glyphs=%u intervals=%u bpp=%u line_h=%u", path,
             static_cast<unsigned>(hdr.glyph_count), static_cast<unsigned>(hdr.interval_count), bpp,
             hdr.line_height);
    return f;
}

void epdfont_close(epdfont_t* font) {
    if (font == nullptr) {
        return;
    }
    if (font->fp != nullptr) {
        std::fclose(font->fp);
        font->fp = nullptr;
    }
    if (font->intervals) {
        heap_caps_free(font->intervals);
        font->intervals = nullptr;
    }
    if (font->glyphs) {
        heap_caps_free(font->glyphs);
        font->glyphs = nullptr;
    }
    if (font->page_bitmap_buf) {
        heap_caps_free(font->page_bitmap_buf);
        font->page_bitmap_buf = nullptr;
    }
    if (font->page_slots) {
        heap_caps_free(font->page_slots);
        font->page_slots = nullptr;
    }
    for (size_t i = 0; i < kOverflowSlots; ++i) {
        if (font->overflow[i].data) {
            heap_caps_free(font->overflow[i].data);
            font->overflow[i].data = nullptr;
        }
    }
    delete font;
}

const lv_font_t* epdfont_get_lv_font(const epdfont_t* font) {
    return font != nullptr ? &font->lv : nullptr;
}

uint8_t epdfont_get_bpp(const epdfont_t* font) {
    return font != nullptr ? font->bpp : 0;
}

uint32_t epdfont_glyph_count(const epdfont_t* font) {
    return font != nullptr ? font->hdr.glyph_count : 0;
}

void epdfont_clear_page_cache(epdfont_t* font) {
    if (font == nullptr) {
        return;
    }
    font->page_bitmap_used = 0;
    font->page_slot_count = 0;
}

bool epdfont_prewarm_utf8(epdfont_t* font, const char* utf8, size_t len) {
    if (font == nullptr || utf8 == nullptr) {
        return false;
    }
    std::vector<uint32_t> cps;
    Utf8CollectCodepoints(utf8, len, cps);
    if (cps.empty()) {
        return true;
    }

    struct Need {
        uint32_t gi;
        uint32_t bmp_off;
        uint16_t bmp_len;
    };
    std::vector<Need> needs;
    needs.reserve(std::min(cps.size(), kMaxPageGlyphs));

    for (uint32_t cp : cps) {
        uint32_t gi = 0;
        if (!LookupGlyphIndex(font, cp, &gi)) {
            continue;
        }
        const epdfont_glyph_t& g = font->glyphs[gi];
        if (g.bmp_len == 0) {
            continue;
        }
        needs.push_back(Need{gi, g.bmp_off, g.bmp_len});
        if (needs.size() >= kMaxPageGlyphs) {
            break;
        }
    }

    // 去重后按文件偏移排序，便于顺序 SD 读
    std::sort(needs.begin(), needs.end(),
              [](const Need& a, const Need& b) { return a.gi < b.gi; });
    needs.erase(std::unique(needs.begin(), needs.end(),
                            [](const Need& a, const Need& b) { return a.gi == b.gi; }),
                needs.end());
    std::sort(needs.begin(), needs.end(),
              [](const Need& a, const Need& b) { return a.bmp_off < b.bmp_off; });

    epdfont_clear_page_cache(font);
    for (const Need& n : needs) {
        if (font->page_slot_count >= kMaxPageGlyphs) {
            break;
        }
        if (font->page_bitmap_used + n.bmp_len > font->page_bitmap_cap) {
            ESP_LOGW(TAG, "page bitmap budget full (%u glyphs cached)",
                     static_cast<unsigned>(font->page_slot_count));
            break;
        }
        uint8_t* dst = font->page_bitmap_buf + font->page_bitmap_used;
        const uint32_t file_off = font->hdr.bitmaps_offset + n.bmp_off;
        if (!SeekRead(font->fp, file_off, dst, n.bmp_len)) {
            ESP_LOGW(TAG, "prewarm read fail gi=%u", static_cast<unsigned>(n.gi));
            continue;
        }
        PageSlot& slot = font->page_slots[font->page_slot_count++];
        slot.glyph_index = n.gi;
        slot.bmp_len = n.bmp_len;
        slot.buf_off = static_cast<uint32_t>(font->page_bitmap_used);
        font->page_bitmap_used += n.bmp_len;
    }

    ESP_LOGD(TAG, "prewarm %u glyphs, %u bytes", static_cast<unsigned>(font->page_slot_count),
             static_cast<unsigned>(font->page_bitmap_used));
    return true;
}
