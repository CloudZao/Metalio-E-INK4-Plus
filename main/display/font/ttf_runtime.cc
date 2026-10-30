#include "ttf_runtime.h"
#include "ttf_face.h"
#include "sd_access.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "ttf_runtime";
constexpr size_t kMetricCount = 2048;
constexpr size_t kAdvanceCount = 65536; // BMP 码点的准确步进缓存
constexpr uint16_t kAdvanceUnknown = UINT16_MAX; // 尚未测量，回退完整字形度量
constexpr uint16_t kAdvanceMissing = UINT16_MAX - 1; // 字体缺字，保留 LVGL 回退行为
constexpr size_t kBitmapCount = 1024;
constexpr size_t kBitmapBudget = 384 * 1024;
constexpr size_t kCacheWays = 4;
constexpr size_t kPrewarmBatch = 32;
constexpr uint32_t kWorkerStack = 48 * 1024;
constexpr int64_t kErrorLogIntervalUs = 5 * 1000 * 1000;
enum class Operation {
    kOpen, // 打开字体
    kMetrics, // 查询字形度量
    kBitmap, // 读取字形位图
    kPrewarm, // 批量预热码点
    kClose, // 关闭字体
};

struct Metric {
    uint32_t codepoint = UINT32_MAX; // 缓存码点；UINT32_MAX 表示无效
    uint32_t glyph = 0; // 0 表示源字体缺字
    uint16_t advance = 0; // 水平步进像素
    uint16_t width = 0, height = 0; // 栅格化后的字形边界
    int16_t left = 0, bottom = 0; // 相对基线的偏移
};

struct Bitmap {
    uint32_t glyph = 0; // 缓存字形索引
    uint16_t width = 0, height = 0; // 紧密排列的 A8 位图尺寸
    uint64_t position = 0; // 环形缓冲中的单调写入位置
};

struct Request {
    Operation operation; // 同步字体操作
    const char* path = nullptr; // 仅打开时使用
    int size_px = 0; // 目标像素字号
    uint32_t glyph = 0; // 度量时为码点，绘制时为字形索引
    Metric* metric = nullptr; // 度量输出
    lv_draw_buf_t* bitmap = nullptr; // 调用方提供的 A8 缓冲
    const uint32_t* codepoints = nullptr; // 预热时使用的码点列表
    size_t count = 0; // 待预热码点数量
    bool ok = false; // 操作结果
};
} // namespace

struct TtfRuntime {
    lv_font_t font{}; // 提供给正文渲染和 LVGL 的字体
    lv_font_t layout_font{}; // 仅提供准确步进的分页字体视图
    uint16_t* advances = nullptr; // BMP 步进缓存，可选 PSRAM 128 KiB
    FT_Library library = nullptr; // 仅字体任务访问
    FT_Face face = nullptr; // 仅字体任务访问
    uint32_t loaded_glyph = 0; // FreeType 当前已加载的字形
    bool rendered = false; // 当前字形是否已生成位图
    std::atomic<bool> io_failed{false}; // 本次 FreeType 操作的文件读取状态
    QueueHandle_t queue = nullptr; // 同步请求队列
    SemaphoreHandle_t done = nullptr; // 单个请求的完成通知
    TaskHandle_t task = nullptr; // 使用独立大栈的字体任务
    std::mutex mutex; // 串行化 UI 和排版任务的请求与缓存访问
    Metric* metrics = nullptr; // PSRAM 中的有界字形度量缓存
    Bitmap* bitmaps = nullptr; // PSRAM 中的位图缓存索引
    uint8_t* pixels = nullptr; // 有容量上限的 A8 环形缓冲
    uint8_t metric_next[kMetricCount / kCacheWays]{}; // 每组度量缓存的轮换淘汰位置
    uint8_t bitmap_slot_next[kBitmapCount / kCacheWays]{}; // 每组位图缓存的轮换淘汰位置
    uint64_t bitmap_next = 0; // 下一次位图写入的单调位置
    int64_t error_log_us = -kErrorLogIntervalUs; // 上次错误日志时间
    unsigned suppressed_errors = 0; // 当前限频窗口内抑制的错误数量
};

namespace {
void ReportError(TtfRuntime& font, const char* operation, FT_Error error, uint32_t glyph = 0) {
    if (sd_access::IsQuiescing()) return;
    const int64_t now = esp_timer_get_time();
    if (now - font.error_log_us < kErrorLogIntervalUs) {
        ++font.suppressed_errors;
        return;
    }
    ESP_LOGW(TAG, "%s failed error=0x%x glyph=%lu suppressed=%u", operation,
             static_cast<unsigned>(error), static_cast<unsigned long>(glyph), font.suppressed_errors);
    font.error_log_us = now;
    font.suppressed_errors = 0;
}

size_t CacheIndex(uint32_t key, size_t count) {
    const uint32_t hash = key * 2654435761u;
    return ((static_cast<uint64_t>(hash) * (count / kCacheWays)) >> 32) * kCacheWays;
}

bool LoadGlyph(TtfRuntime& font, uint32_t glyph, bool render) {
    font.io_failed.store(false);
    FT_Error error = 0;
    if (font.loaded_glyph != glyph) {
        font.loaded_glyph = 0;
        font.rendered = false;
        // FreeType 预计算位图边界，分页度量不执行栅格化。
        error = FT_Load_Glyph(font.face, glyph, FT_LOAD_BITMAP_METRICS_ONLY | FT_LOAD_TARGET_NORMAL);
        if (error == 0 && !font.io_failed.load()) font.loaded_glyph = glyph;
    }
    if (error == 0 && !font.io_failed.load() && render && !font.rendered) {
        // 位图字体的 metrics-only 加载不持有像素，需重新读取该字形。
        if (font.face->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            error = FT_Load_Glyph(font.face, glyph, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
        } else {
            error = FT_Render_Glyph(font.face->glyph, FT_RENDER_MODE_NORMAL);
        }
        font.rendered = error == 0 && !font.io_failed.load();
    }
    if (font.io_failed.load()) error = FT_Err_Invalid_Stream_Read;
    if (error != 0) {
        font.loaded_glyph = 0;
        font.rendered = false;
        ReportError(font, render ? "render glyph" : "load metrics", error, glyph);
        return false;
    }
    return true;
}

Metric* FindMetric(TtfRuntime& font, uint32_t codepoint) {
    const size_t base = CacheIndex(codepoint, kMetricCount);
    for (size_t i = 0; i < kCacheWays; ++i) {
        auto& metric = font.metrics[base + i];
        if (metric.codepoint == codepoint) return &metric;
    }
    return nullptr;
}

void CacheAdvance(TtfRuntime& font, const Metric& metric) {
    if (!font.advances || metric.codepoint >= kAdvanceCount) return;
    if (metric.glyph == 0) font.advances[metric.codepoint] = kAdvanceMissing;
    else if (metric.advance < kAdvanceMissing) font.advances[metric.codepoint] = metric.advance;
}

Metric* LoadMetric(TtfRuntime& font, uint32_t codepoint) {
    if (auto* cached = FindMetric(font, codepoint)) return cached;
    font.io_failed.store(false);
    const auto glyph = FT_Get_Char_Index(font.face, codepoint);
    if (font.io_failed.load()) {
        ReportError(font, "lookup character", FT_Err_Invalid_Stream_Read);
        return nullptr;
    }
    Metric value{};
    value.codepoint = codepoint;
    value.glyph = glyph;
    if (glyph != 0) {
        if (!LoadGlyph(font, glyph, false)) return nullptr;
        const auto slot = font.face->glyph;
        if (slot->bitmap.width > UINT16_MAX || slot->bitmap.rows > UINT16_MAX) {
            ReportError(font, "glyph bounds", FT_Err_Invalid_Pixel_Size, glyph);
            return nullptr;
        }
        value.advance = std::clamp<long>((slot->advance.x + 32) >> 6, 0, UINT16_MAX);
        value.width = slot->bitmap.width;
        value.height = slot->bitmap.rows;
        value.left = slot->bitmap_left;
        value.bottom = slot->bitmap_top - static_cast<int>(slot->bitmap.rows);
    }
    const size_t base = CacheIndex(codepoint, kMetricCount);
    size_t slot = base + font.metric_next[base / kCacheWays]++ % kCacheWays;
    for (size_t i = 0; i < kCacheWays; ++i) {
        if (font.metrics[base + i].codepoint == UINT32_MAX) { slot = base + i; break; }
    }
    auto& metric = font.metrics[slot];
    metric = value;
    CacheAdvance(font, value);
    return &metric;
}

const Bitmap* FindBitmap(TtfRuntime& font, uint32_t glyph) {
    if (glyph == 0) return nullptr;
    const size_t base = CacheIndex(glyph, kBitmapCount);
    for (size_t i = 0; i < kCacheWays; ++i) {
        const auto& bitmap = font.bitmaps[base + i];
        if (bitmap.glyph == glyph && font.bitmap_next - bitmap.position <= kBitmapBudget) return &bitmap;
    }
    return nullptr;
}

bool CopyBitmap(TtfRuntime& font, uint32_t glyph, const uint8_t* pixels,
                 uint32_t width, uint32_t height, lv_draw_buf_t& output) {
    const uint32_t stride = output.header.stride ? output.header.stride :
        lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_A8);
    const size_t bytes = static_cast<size_t>(stride) * height;
    if (stride < width || bytes > output.data_size) {
        char operation[112];
        std::snprintf(operation, sizeof(operation), "bitmap buffer %ux%u stride=%lu capacity=%lu",
                      static_cast<unsigned>(width), static_cast<unsigned>(height),
                      static_cast<unsigned long>(stride), static_cast<unsigned long>(output.data_size));
        ReportError(font, operation, FT_Err_Array_Too_Large, glyph);
        return false;
    }
    std::memset(output.data, 0, bytes);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(output.data + y * stride, pixels + y * width, width);
    }
    output.header.w = width;
    output.header.h = height;
    output.header.stride = stride;
    output.header.cf = LV_COLOR_FORMAT_A8;
    return true;
}

bool RenderBitmap(TtfRuntime& font, uint32_t glyph, lv_draw_buf_t* output) {
    if (const auto* cached = FindBitmap(font, glyph)) {
        return !output || CopyBitmap(font, glyph, font.pixels + cached->position % kBitmapBudget,
                                     cached->width, cached->height, *output);
    }
    if (!LoadGlyph(font, glyph, true)) return false;
    const auto& source = font.face->glyph->bitmap;
    if (source.pixel_mode != FT_PIXEL_MODE_GRAY && source.pixel_mode != FT_PIXEL_MODE_MONO) {
        ReportError(font, "pixel format", FT_Err_Unimplemented_Feature, glyph);
        return false;
    }
    const size_t bytes = static_cast<size_t>(source.width) * source.rows;
    if (bytes == 0) return true;
    if (bytes > kBitmapBudget || source.width > UINT16_MAX || source.rows > UINT16_MAX) {
        ReportError(font, "bitmap bounds", FT_Err_Invalid_Pixel_Size, glyph);
        return false;
    }
    const size_t offset = font.bitmap_next % kBitmapBudget;
    if (offset + bytes > kBitmapBudget) font.bitmap_next += kBitmapBudget - offset;
    const size_t base = CacheIndex(glyph, kBitmapCount);
    size_t slot = base + font.bitmap_slot_next[base / kCacheWays]++ % kCacheWays;
    for (size_t i = 0; i < kCacheWays; ++i) {
        const auto& entry = font.bitmaps[base + i];
        if (entry.glyph == 0 || font.bitmap_next - entry.position > kBitmapBudget) { slot = base + i; break; }
    }
    auto& cached = font.bitmaps[slot];
    cached = {glyph, static_cast<uint16_t>(source.width), static_cast<uint16_t>(source.rows), font.bitmap_next};
    auto* destination = font.pixels + font.bitmap_next % kBitmapBudget;
    for (unsigned y = 0; y < source.rows; ++y) {
        const auto* row = source.pitch >= 0 ? source.buffer + y * source.pitch :
            source.buffer + (source.rows - 1 - y) * -source.pitch;
        auto* target = destination + y * source.width;
        if (source.pixel_mode == FT_PIXEL_MODE_GRAY) {
            std::memcpy(target, row, source.width);
        } else {
            for (unsigned x = 0; x < source.width; ++x) {
                target[x] = (row[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
            }
        }
    }
    font.bitmap_next += bytes;
    return !output || CopyBitmap(font, glyph, destination, source.width, source.rows, *output);
}

bool Execute(TtfRuntime& font, Request& request) {
    if (request.operation == Operation::kClose) {
        if (font.face) FT_Done_Face(font.face);
        if (font.library) FT_Done_FreeType(font.library);
        font.face = nullptr;
        font.library = nullptr;
        return true;
    }
    if (sd_access::IsQuiescing()) return false;
    if (request.operation == Operation::kOpen) {
        FT_Error error = FT_Init_FreeType(&font.library);
        if (error != 0) {
            font.library = nullptr;
            ReportError(font, "initialize FreeType", error);
            return false;
        }
        if (!TtfOpenFace(font.library, request.path, &font.face, &font.io_failed, &error)) {
            ReportError(font, "open font", error);
            return false;
        }
        error = FT_Set_Pixel_Sizes(font.face, 0, request.size_px);
        if (error != 0) {
            ReportError(font, "set size", error);
            return false;
        }
        const int ascender = font.face->size->metrics.ascender >> 6;
        const int descender = std::max<int>(0, -(font.face->size->metrics.descender >> 6));
        font.font.line_height = std::max(1, ascender + descender);
        font.font.base_line = descender;
        font.font.underline_position = -2;
        font.font.underline_thickness = 1;
        return true;
    }
    if (request.operation == Operation::kMetrics) {
        const auto* metric = LoadMetric(font, request.glyph);
        if (!metric) return false;
        *request.metric = *metric;
        return true;
    }
    if (request.operation == Operation::kPrewarm) {
        int64_t yielded_us = esp_timer_get_time();
        for (size_t i = 0; i < request.count; ++i) {
            if (sd_access::IsQuiescing()) return false;
            const auto* metric = LoadMetric(font, request.codepoints[i]);
            if (!metric) return false;
            if (metric->glyph != 0 && !RenderBitmap(font, metric->glyph, nullptr)) return false;
            if (esp_timer_get_time() - yielded_us >= 20000) {
                vTaskDelay(1);
                yielded_us = esp_timer_get_time();
            }
        }
        return true;
    }
    return RenderBitmap(font, request.glyph, request.bitmap);
}

void Worker(void* context) {
    auto& font = *static_cast<TtfRuntime*>(context);
    Request* request = nullptr;
    while (xQueueReceive(font.queue, &request, portMAX_DELAY) == pdTRUE) {
        request->ok = Execute(font, *request);
        xSemaphoreGive(font.done);
    }
    vTaskDeleteWithCaps(nullptr);
}

bool Send(TtfRuntime& font, Request& request) {
    auto* pointer = &request;
    if (xQueueSend(font.queue, &pointer, portMAX_DELAY) != pdTRUE ||
        xSemaphoreTake(font.done, portMAX_DELAY) != pdTRUE) {
        ReportError(font, "worker synchronization", FT_Err_Invalid_Argument);
        return false;
    }
    return request.ok;
}

bool GetMetrics(const lv_font_t* lv_font, lv_font_glyph_dsc_t* output, uint32_t codepoint,
                 uint32_t /*next*/) {
    auto& font = *static_cast<TtfRuntime*>(const_cast<void*>(lv_font->dsc));
    const bool tab = codepoint == '\t';
    if (tab) codepoint = ' ';
    std::lock_guard<std::mutex> lock(font.mutex);
    Metric value{};
    const Metric* metric = FindMetric(font, codepoint);
    if (!metric) {
        Request request{Operation::kMetrics};
        request.glyph = codepoint;
        request.metric = &value;
        if (!Send(font, request)) return false;
        metric = &value;
    }
    if (metric->glyph == 0) return false;
    *output = {};
    output->adv_w = metric->advance;
    if (tab) output->adv_w = std::min<unsigned>(UINT16_MAX, output->adv_w * 2u);
    output->box_w = metric->width;
    output->box_h = metric->height;
    output->ofs_x = metric->left;
    output->ofs_y = metric->bottom;
    output->format = LV_FONT_GLYPH_FORMAT_A8;
    output->gid.index = metric->glyph;
    return true;
}

bool GetLayoutMetrics(const lv_font_t* lv_font, lv_font_glyph_dsc_t* output,
                      uint32_t codepoint, uint32_t next) {
    auto& font = *static_cast<TtfRuntime*>(const_cast<void*>(lv_font->dsc));
    const bool tab = codepoint == '\t';
    const uint32_t key = tab ? ' ' : codepoint;
    if (font.advances && key < kAdvanceCount) {
        std::lock_guard<std::mutex> lock(font.mutex);
        const uint16_t advance = font.advances[key];
        if (advance == kAdvanceMissing) return false;
        if (advance != kAdvanceUnknown) {
            *output = {};
            output->adv_w = tab ? std::min<unsigned>(UINT16_MAX, advance * 2u) : advance;
            output->format = LV_FONT_GLYPH_FORMAT_A8;
            return true;
        }
    }
    return GetMetrics(lv_font, output, codepoint, next);
}

const void* GetBitmap(lv_font_glyph_dsc_t* dsc, lv_draw_buf_t* output) {
    if (!output || !output->data || dsc->req_raw_bitmap || dsc->box_w == 0 || dsc->box_h == 0) return nullptr;
    auto& font = *static_cast<TtfRuntime*>(const_cast<void*>(dsc->resolved_font->dsc));
    std::lock_guard<std::mutex> lock(font.mutex);
    bool ok = false;
    if (const auto* cached = FindBitmap(font, dsc->gid.index)) {
        ok = CopyBitmap(font, dsc->gid.index, font.pixels + cached->position % kBitmapBudget,
                        cached->width, cached->height, *output);
    } else {
        Request request{Operation::kBitmap};
        request.glyph = dsc->gid.index;
        request.bitmap = output;
        ok = Send(font, request);
    }
    if (!ok) return nullptr;
    lv_draw_buf_flush_cache(output, nullptr);
    return output;
}
} // namespace

TtfRuntime* TtfRuntimeOpen(const char* path, int size_px) {
    if (!path || size_px < 20 || size_px > 80) return nullptr;
    auto* font = new(std::nothrow) TtfRuntime;
    if (!font) {
        ESP_LOGE(TAG, "font context allocation failed size=%d", size_px);
        return nullptr;
    }
    font->metrics = static_cast<Metric*>(heap_caps_malloc(kMetricCount * sizeof(Metric), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    font->bitmaps = static_cast<Bitmap*>(heap_caps_calloc(kBitmapCount, sizeof(Bitmap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    font->pixels = static_cast<uint8_t*>(heap_caps_malloc(kBitmapBudget, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    font->queue = xQueueCreate(1, sizeof(Request*));
    font->done = xSemaphoreCreateBinary();
    if (!font->metrics || !font->bitmaps || !font->pixels || !font->queue || !font->done) {
        ESP_LOGE(TAG, "font cache or synchronization allocation failed size=%d", size_px);
        TtfRuntimeClose(font);
        return nullptr;
    }
    font->advances = static_cast<uint16_t*>(heap_caps_malloc(kAdvanceCount * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (font->advances) std::memset(font->advances, 0xFF, kAdvanceCount * sizeof(uint16_t));
    for (size_t i = 0; i < kMetricCount; ++i) font->metrics[i] = {};
    // FreeType 栅格化统一放在独立 PSRAM 栈，避免增加 UI 和所有排版任务的栈。
    if (xTaskCreatePinnedToCoreWithCaps(Worker, "book_ttf", kWorkerStack, font, tskIDLE_PRIORITY + 2,
            &font->task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "font worker creation failed stack=%lu", static_cast<unsigned long>(kWorkerStack));
        TtfRuntimeClose(font);
        return nullptr;
    }
    Request request{Operation::kOpen};
    request.path = path;
    request.size_px = size_px;
    if (!Send(*font, request)) {
        if (!sd_access::IsQuiescing()) ESP_LOGW(TAG, "cannot load font %s at %dpx", path, size_px);
        TtfRuntimeClose(font);
        return nullptr;
    }
    font->font.dsc = font;
    font->font.get_glyph_dsc = GetMetrics;
    font->font.get_glyph_bitmap = GetBitmap;
    font->layout_font = font->font;
    font->layout_font.get_glyph_dsc = GetLayoutMetrics;
    font->layout_font.get_glyph_bitmap = nullptr;
    return font;
}

void TtfRuntimeClose(TtfRuntime* font) {
    if (!font) return;
    if (font->task) {
        Request request{Operation::kClose};
        Send(*font, request);
        vTaskDeleteWithCaps(font->task);
    }
    if (font->queue) vQueueDelete(font->queue);
    if (font->done) vSemaphoreDelete(font->done);
    heap_caps_free(font->advances);
    heap_caps_free(font->metrics);
    heap_caps_free(font->bitmaps);
    heap_caps_free(font->pixels);
    delete font;
}

const lv_font_t* TtfRuntimeFont(const TtfRuntime* font) {
    return font ? &font->font : nullptr;
}

const lv_font_t* TtfRuntimeLayoutFont(const TtfRuntime* font) {
    return font ? (font->advances ? &font->layout_font : &font->font) : nullptr;
}

bool TtfRuntimePrewarm(TtfRuntime* font, const uint32_t* codepoints, size_t count) {
    if (!font || (!codepoints && count != 0)) return false;
    for (size_t begin = 0; begin < count; begin += kPrewarmBatch) {
        std::lock_guard<std::mutex> lock(font->mutex);
        if (sd_access::IsQuiescing()) return false;
        const size_t batch = std::min(kPrewarmBatch, count - begin);
        bool ready = true;
        for (size_t i = 0; i < batch; ++i) {
            const auto* metric = FindMetric(*font, codepoints[begin + i]);
            if (!metric || (metric->glyph != 0 && metric->width != 0 && metric->height != 0 &&
                            !FindBitmap(*font, metric->glyph))) {
                ready = false;
                break;
            }
        }
        if (ready) continue;
        Request request{Operation::kPrewarm};
        request.codepoints = codepoints + begin;
        request.count = batch;
        if (!Send(*font, request)) return false;
    }
    return true;
}
