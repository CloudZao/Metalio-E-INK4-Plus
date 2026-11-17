#include "fontpack_lvgl.h"

#include "font_loader.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "font_bitmap_expand.h"

#include <cstring>
#include <mutex>

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
    lv_font_t sd{}; // 与主字库同规格的 SD 回退字体
    uint16_t size = 0;
    uint16_t bpp = 0;
    bool inited = false;
};

// UI 30@2（397=25@2，字库取最近档）+ 百问AI 30@4 + Math 18/28/36@2
constexpr int kMaxFonts = 12;
FontpackLvFont s_fonts[kMaxFonts];
std::mutex s_fonts_mutex;
bool s_flash_tried = false;
bool s_flash_ok = false;

using font_bitmap_expand::UnpackToA8;

struct SdGlyphCacheEntry {
    uint64_t key; // 字符、字号和灰度组合
    TickType_t loaded; // 缓存有效期起点
    int result; // 成功或正常缺字
    GlyphData glyph; // 单字位图，不向 LVGL 暴露缓存地址
};

static bool GetSdGlyph(uint32_t letter, uint16_t size, uint16_t bpp, GlyphData* glyph) {
    static std::mutex mutex;
    static SdGlyphCacheEntry* cache = nullptr;
    static unsigned next = 0;
    static unsigned generation = 0;
    static TickType_t retry_at = 0;
    static bool retry = false;
    static int last_error = FONT_LOADER_OK;
    static bool available = false;
    constexpr unsigned capacity = 16;
    constexpr TickType_t ttl = pdMS_TO_TICKS(5000);
    std::lock_guard<std::mutex> lock(mutex);
    sd_access::Transaction transaction;
    if (!transaction) return false;
    const unsigned mounted = sd_access::MountGeneration();
    if (generation != mounted) {
        if (cache) std::memset(cache, 0, sizeof(*cache) * capacity);
        generation = mounted;
        retry = false;
        last_error = FONT_LOADER_OK;
        available = false;
    }
    const TickType_t now = xTaskGetTickCount();
    if (retry && now - retry_at < ttl) return false;
    const uint64_t key = fontpack_make_key(letter, size, bpp);
    for (unsigned i = 0; cache && i < capacity; ++i) {
        if (cache[i].key == key && now - cache[i].loaded < ttl) {
            if (cache[i].result != FONT_LOADER_OK) return false;
            *glyph = cache[i].glyph;
            return true;
        }
    }
    const int err = font_loader_read_sd(SD_PATH_FONT_EXT, letter, size, bpp, glyph);
    if (err != FONT_LOADER_OK && err != FONT_LOADER_ERR_NOT_FOUND) {
        // 缺少可选文件保持静默；损坏及 I/O 原因已在读取层记录，延后重试避免刷屏。
        retry = true;
        retry_at = now;
        last_error = err;
        available = false;
        return false;
    }
    if (!cache) {
        cache = static_cast<SdGlyphCacheEntry*>(heap_caps_calloc(
            capacity, sizeof(*cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!cache) {
            if (last_error != FONT_LOADER_ERR_STATE) ESP_LOGE(TAG, "SD glyph cache allocation failed");
            last_error = FONT_LOADER_ERR_STATE;
            retry = true;
            retry_at = now;
            return false;
        }
    }
    if (!available) ESP_LOGI(TAG, "SD extension font available");
    available = true;
    last_error = FONT_LOADER_OK;
    retry = false;
    auto& slot = cache[next++ % capacity];
    slot.glyph = *glyph;
    slot.key = key;
    slot.loaded = now;
    slot.result = err;
    return err == FONT_LOADER_OK;
}

static bool LookupGlyph(const lv_font_t* font, uint32_t letter, GlyphInfo* glyph, GlyphData* sd) {
    auto* f = static_cast<const FontpackLvFont*>(font->dsc);
    if (font->static_bitmap) return get_glyph(letter, f->size, f->bpp, glyph);
    if (!GetSdGlyph(letter, f->size, f->bpp, sd)) return false;
    *glyph = {sd->width, sd->height, sd->x_offset, sd->y_offset, sd->advance, sd->bmp_len, sd->bitmap};
    return true;
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
    if (f == nullptr || dsc_out == nullptr || (font->static_bitmap && !font_loader_is_ready())) {
        return false;
    }
    if (letter == '\t') {
        letter = ' ';
    }
    GlyphInfo g{};
    GlyphData sd;
    if (!LookupGlyph(font, letter, &g, &sd)) {
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
    dsc_out->gid.index = letter;  // 位图回调按同一字符查询 Flash 或 SD 缓存
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
    GlyphData sd;
    if (!LookupGlyph(g_dsc->resolved_font, g_dsc->gid.index, &g, &sd)) {
        return nullptr;
    }
    if (g.width != g_dsc->box_w || g.height != g_dsc->box_h ||
        g.width == 0 || g.height == 0 || g.bmp_len == 0 || g.bitmap == nullptr) {
        return nullptr;
    }

    if (g_dsc->req_raw_bitmap) {
        return g_dsc->resolved_font->static_bitmap ? g.bitmap : nullptr;
    }
    if (draw_buf == nullptr || draw_buf->data == nullptr) {
        return nullptr;
    }
    const uint32_t stride_out = draw_buf->header.stride != 0
        ? draw_buf->header.stride : lv_draw_buf_width_to_stride(g.width, LV_COLOR_FORMAT_A8);
    const size_t need = static_cast<size_t>(stride_out) * g.height;
    if (stride_out < g.width || need > draw_buf->data_size) {
        return nullptr;
    }
    std::memset(draw_buf->data, 0, need);
    UnpackToA8(g.bitmap, static_cast<uint8_t>(f->bpp), g.width, g.height, draw_buf->data,
               stride_out);
    draw_buf->header.w = g.width;
    draw_buf->header.h = g.height;
    draw_buf->header.cf = LV_COLOR_FORMAT_A8;
    draw_buf->header.stride = stride_out;
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
    std::lock_guard<std::mutex> lock(s_fonts_mutex);
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

    *slot = FontpackLvFont{};
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
    slot->lv.fallback = &slot->sd;
    slot->lv.user_data = nullptr;
    if (font_loader_is_ready()) {
        EstimateMetrics(slot);
    } else {
        slot->lv.line_height = size + size / 5;
        slot->lv.base_line = size / 5;
    }
    slot->sd = slot->lv;
    slot->sd.static_bitmap = 0;
    slot->sd.fallback = nullptr;
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

const lv_font_t* fontpack_lv_font_sd(uint16_t size, uint16_t bpp) {
    if (size == 0 || (bpp != 1 && bpp != 2 && bpp != 4)) return nullptr;
    FontpackLvFont* slot = FindOrCreate(size, bpp);
    return slot != nullptr ? &slot->sd : nullptr;
}

uint16_t fontpack_lv_font_bpp(const lv_font_t* font) {
    if (font == nullptr || font->dsc == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_fonts_mutex);
    // dsc 指向 FontpackLvFont（lv 为首成员）
    for (int i = 0; i < kMaxFonts; ++i) {
        if (s_fonts[i].inited && (&s_fonts[i].lv == font || &s_fonts[i].sd == font)) {
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
