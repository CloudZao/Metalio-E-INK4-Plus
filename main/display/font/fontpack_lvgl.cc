#include "fontpack_lvgl.h"

#include "font_loader.h"

#include <cstring>

#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "fontpack_lv";

struct FontpackLvFont {
    lv_font_t lv{};
    uint16_t size = 0;
    uint16_t bpp = 0;
    bool inited = false;
};

// UI 30@2（397=25@2，字库取最近档）+ 百问AI 30@4 + Math 18/28/36@2
constexpr int kMaxFonts = 12;
FontpackLvFont s_fonts[kMaxFonts];
bool s_flash_tried = false;
bool s_flash_ok = false;

static const uint8_t kOpa2Table[4] = {0, 85, 170, 255};
static const uint8_t kOpa4Table[16] = {0,  17, 34,  51,  68,  85,  102, 119,
                                      136, 153, 170, 187, 204, 221, 238, 255};

void UnpackToA8(const uint8_t* src, uint8_t bpp, uint16_t box_w, uint16_t box_h, uint8_t* dst,
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
    if (bpp == 2) {
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
        return;
    }
    // bpp == 4
    uint32_t pix_i = 0;
    for (uint16_t y = 0; y < box_h; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * stride_out;
        for (uint16_t x = 0; x < box_w; ++x) {
            const uint8_t byte = src[pix_i >> 1];
            const uint8_t nibble = (pix_i & 1) ? (byte & 0x0f) : (byte >> 4);
            row[x] = kOpa4Table[nibble & 0x0f];
            ++pix_i;
        }
    }
}

lv_font_glyph_format_t FormatForBpp(uint16_t bpp) {
    if (bpp <= 1) {
        return LV_FONT_GLYPH_FORMAT_A1;
    }
    if (bpp == 2) {
        return LV_FONT_GLYPH_FORMAT_A2;
    }
    return LV_FONT_GLYPH_FORMAT_A4;
}

bool GetGlyphDscCb(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t letter,
                   uint32_t /*letter_next*/) {
    auto* f = static_cast<FontpackLvFont*>(const_cast<void*>(font->dsc));
    if (f == nullptr || dsc_out == nullptr || !font_loader_is_ready()) {
        return false;
    }
    if (letter == '\t') {
        letter = ' ';
    }
    GlyphInfo g{};
    if (!get_glyph(letter, f->size, f->bpp, &g)) {
        return false;
    }
    dsc_out->adv_w = g.advance;
    if (letter == ' ' || letter == '\t') {
        // 空格可能无位图，仍要有步进
        if (dsc_out->adv_w == 0) {
            dsc_out->adv_w = f->size / 3;
        }
    }
    dsc_out->box_w = g.width;
    dsc_out->box_h = g.height;
    dsc_out->ofs_x = g.x_offset;
    dsc_out->ofs_y = g.y_offset;
    dsc_out->stride = 0;
    dsc_out->format = FormatForBpp(f->bpp);
    dsc_out->is_placeholder = 0;
    dsc_out->gid.index = letter;  // bitmap 回调再查一次（mmap 二分很便宜）
    return true;
}

const void* GetGlyphBitmapCb(lv_font_glyph_dsc_t* g_dsc, lv_draw_buf_t* draw_buf) {
    if (g_dsc == nullptr || g_dsc->resolved_font == nullptr) {
        return nullptr;
    }
    auto* f = static_cast<FontpackLvFont*>(const_cast<void*>(g_dsc->resolved_font->dsc));
    if (f == nullptr || g_dsc->gid.index == 0) {
        return nullptr;
    }
    GlyphInfo g{};
    if (!get_glyph(g_dsc->gid.index, f->size, f->bpp, &g)) {
        return nullptr;
    }
    if (g.width == 0 || g.height == 0 || g.bmp_len == 0 || g.bitmap == nullptr) {
        return nullptr;
    }

    if (g_dsc->req_raw_bitmap) {
        return g.bitmap;
    }
    if (draw_buf == nullptr || draw_buf->data == nullptr) {
        return nullptr;
    }
    const uint32_t stride_out = lv_draw_buf_width_to_stride(g.width, LV_COLOR_FORMAT_A8);
    UnpackToA8(g.bitmap, static_cast<uint8_t>(f->bpp), g.width, g.height, draw_buf->data,
               stride_out);
    lv_draw_buf_flush_cache(draw_buf, nullptr);
    return draw_buf;
}

void EstimateMetrics(FontpackLvFont* f) {
    // 用若干典型字估算行高 / 基线
    int16_t max_ascent = 0;
    int16_t max_descent = 0;
    const uint32_t probes[] = {0x0048u, 0x0067u, 0x4F60u, 0x56FDu, 0x0041u, 0x0079u};  // H g 你 国 A y
    for (uint32_t cp : probes) {
        GlyphInfo g{};
        if (!get_glyph(cp, f->size, f->bpp, &g) || g.height == 0) {
            continue;
        }
        // y_offset = bitmap_top - height → 顶相对基线 = y_offset + height
        const int16_t top = static_cast<int16_t>(g.y_offset + g.height);
        const int16_t bottom = g.y_offset;  // 盒底相对基线（常 ≤0）
        if (top > max_ascent) {
            max_ascent = top;
        }
        if (bottom < max_descent) {
            max_descent = bottom;
        }
    }
    if (max_ascent <= 0) {
        f->lv.line_height = static_cast<uint16_t>(f->size + f->size / 5);
        f->lv.base_line = static_cast<int16_t>(f->size / 5);
        return;
    }
    f->lv.line_height = static_cast<uint16_t>(max_ascent - max_descent + 2);
    f->lv.base_line = static_cast<int16_t>(-max_descent + 1);
}

FontpackLvFont* FindOrCreate(uint16_t size, uint16_t bpp) {
    for (int i = 0; i < kMaxFonts; ++i) {
        if (s_fonts[i].inited && s_fonts[i].size == size && s_fonts[i].bpp == bpp) {
            return &s_fonts[i];
        }
    }
    FontpackLvFont* slot = nullptr;
    for (int i = 0; i < kMaxFonts; ++i) {
        if (!s_fonts[i].inited) {
            slot = &s_fonts[i];
            break;
        }
    }
    if (slot == nullptr) {
        ESP_LOGE(TAG, "no free font slot");
        return nullptr;
    }

    std::memset(slot, 0, sizeof(*slot));
    slot->size = size;
    slot->bpp = bpp;
    slot->lv.get_glyph_dsc = GetGlyphDscCb;
    slot->lv.get_glyph_bitmap = GetGlyphBitmapCb;
    slot->lv.release_glyph = nullptr;
    slot->lv.subpx = LV_FONT_SUBPX_NONE;
    slot->lv.kerning = LV_FONT_KERNING_NONE;
    slot->lv.static_bitmap = 1;  // mmap 指针稳定
    slot->lv.underline_position = -1;
    slot->lv.underline_thickness = 1;
    slot->lv.dsc = slot;
    slot->lv.fallback = nullptr;
    slot->lv.user_data = nullptr;
    EstimateMetrics(slot);
    slot->inited = true;
    ESP_LOGI(TAG, "lv_font size=%u bpp=%u line_h=%u base=%d", size, bpp, slot->lv.line_height,
             static_cast<int>(slot->lv.base_line));
    return slot;
}

}  // namespace

namespace {

struct MmapJob {
    SemaphoreHandle_t done = nullptr;
    int err = FONT_LOADER_ERR_IO;
};

void FontpackMmapTask(void* arg) {
    auto* job = static_cast<MmapJob*>(arg);
    job->err = font_loader_init_flash(nullptr);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

}  // namespace

bool fontpack_lv_ensure_ready(void) {
    // LVGL worker 栈在 PSRAM；esp_partition_mmap 冻结 cache 时要求当前栈在内部 DRAM。
    // 因此 mmap 一律丢到短命内部栈任务里做。
    static SemaphoreHandle_t s_lock = nullptr;
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == nullptr) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_flash_tried) {
        const bool ok = s_flash_ok;
        xSemaphoreGive(s_lock);
        return ok;
    }
    s_flash_tried = true;

    MmapJob job{};
    job.done = xSemaphoreCreateBinary();
    if (job.done == nullptr) {
        ESP_LOGE(TAG, "mmap sem alloc fail");
        s_flash_ok = false;
        xSemaphoreGive(s_lock);
        return false;
    }

    // xTaskCreate 默认栈在内部 RAM（非 PSRAM）
    const BaseType_t created =
        xTaskCreate(FontpackMmapTask, "fp_mmap", 4096, &job, tskIDLE_PRIORITY + 5, nullptr);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "fp_mmap task create fail; try direct if stack in DRAM");
        const void* sp = reinterpret_cast<const void*>(esp_cpu_get_sp());
        if (esp_ptr_in_dram(sp)) {
            job.err = font_loader_init_flash(nullptr);
        } else {
            job.err = FONT_LOADER_ERR_STATE;
        }
    } else if (xSemaphoreTake(job.done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "fp_mmap timeout");
        job.err = FONT_LOADER_ERR_IO;
    }
    vSemaphoreDelete(job.done);

    s_flash_ok = (job.err == FONT_LOADER_OK);
    if (!s_flash_ok) {
        ESP_LOGW(TAG, "font_loader_init_flash failed: %d", job.err);
    } else {
        ESP_LOGI(TAG, "fontpack ready items=%lu",
                 static_cast<unsigned long>(font_loader_item_count()));
    }
    xSemaphoreGive(s_lock);
    return s_flash_ok;
}

const lv_font_t* fontpack_lv_font_get(uint16_t size, uint16_t bpp) {
    if (!fontpack_lv_ensure_ready()) {
        return nullptr;
    }
    // 冒烟：确认该规格在包内
    GlyphInfo g{};
    if (!get_glyph(0x4E2Du /* 中 */, size, bpp, &g) && !get_glyph(0x0041u /* A */, size, bpp, &g)) {
        ESP_LOGW(TAG, "no glyphs for size=%u bpp=%u", size, bpp);
        return nullptr;
    }
    FontpackLvFont* slot = FindOrCreate(size, bpp);
    return slot != nullptr ? &slot->lv : nullptr;
}

uint16_t fontpack_lv_font_bpp(const lv_font_t* font) {
    if (font == nullptr || font->dsc == nullptr) {
        return 0;
    }
    // dsc 指向 FontpackLvFont（lv 为首成员）
    for (int i = 0; i < kMaxFonts; ++i) {
        if (s_fonts[i].inited && &s_fonts[i].lv == font) {
            return s_fonts[i].bpp;
        }
    }
    return 0;
}

const lv_font_t* fontpack_lv_font_ui(void) {
    static const lv_font_t* s_ui = nullptr;
    if (s_ui != nullptr) {
        return s_ui;
    }
    s_ui = fontpack_lv_font_get(30, 2);
    if (s_ui == nullptr) {
        ESP_LOGE(TAG, "fontpack UI 30@2 unavailable");
    } else {
        ESP_LOGI(TAG, "UI default font size=30 bpp=2");
    }
    return s_ui;
}
