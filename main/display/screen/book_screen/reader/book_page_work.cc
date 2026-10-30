#include "book_screen/book_geom.h"
#include "book_page_work.h"

#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "reader/page_text_raster.h"
#include "reader/reader_page_render.h"
#include "sd_access.h"

#include <atomic>
#include <new>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

namespace {
constexpr const char* kTag = "ReaderWork";
std::atomic<ReaderPageResult*> completed{nullptr};

struct Work {
    std::unique_ptr<ReaderPageResult> result; // 完成后转交 UI 的结果
    const lv_font_t* font = nullptr; // 工作期间保持存活的正文字体
    int width = 0, height = 0, underline = 0; // 分页视口和下划线模式
    int background = 0, background_left = 0, screen_width = 0; // 背景模式与屏幕坐标
    bool prepare_background = false; // 背景几何变化时生成新图层
    bool antialias = false, vertical = false, rtl = false; // 抗锯齿、竖排和列方向
};

void PreparePage(void* data) {
    auto* work = static_cast<Work*>(data);
    auto& result = *work->result;
    const auto started = esp_timer_get_time();
    result.queue_us = started - result.started_us;
    try {
        sd_access::Permit permit;
        result.ok = result.cursor->PrepareNavigation(result.kind, result.value);
        const auto navigated = esp_timer_get_time();
        result.navigation_us = navigated - started;
        if (result.ok) {
            const auto* page = result.cursor->CurrentPageData();
            PrewarmPageFont(page);
            if (page && !result.cursor->IsPageImagesMode() && reader::PageIsTextOnly(*page) &&
                work->font && work->width > 40 && work->height > 40) {
                lv_coord_t width = 0;
                for (const auto& item : page->items) {
                    if (item.kind != reader::ContentKind::kText || item.text.empty()) continue;
                    lv_point_t size{};
                    lv_text_get_size(&size, item.text.c_str(), work->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                    width = std::max<lv_coord_t>(width, size.x + (item.para_indent ? reader::ParaIndentPadPx(work->font) : 0));
                }
                result.column_width = !work->vertical && width > 40 && width < work->width ? width : work->width;
                const bool raster_ok = work->vertical
                    ? reader::RasterPageVerticalTextToL8(*page, work->font, result.cursor->LineSpacingPercent(), result.cursor->ParaGap(),
                        result.column_width, work->height, work->underline, result.raster, work->antialias, work->rtl)
                    : reader::RasterPageTextToL8(*page, work->font, result.cursor->LineSpacingPercent(), result.cursor->ParaGap(),
                        result.column_width, work->height, work->underline, result.raster, work->antialias);
                if (raster_ok) result.ink_height = result.raster.height;
                if (raster_ok && work->background) {
                    const int left = work->background_left + (work->width - result.column_width) / 2;
                    for (int y = 0; y < result.raster.height; ++y) for (int x = 0; x < result.raster.width; ++x) {
                        auto& pixel = result.raster.pixels[y * result.raster.width + x];
                        if (pixel == 255) pixel = reader::ReadingBackgroundPixel(work->background, left + x, y,
                            work->background_left, work->width, work->height);
                    }
                }
                if (!raster_ok) ESP_LOGW(kTag, "text raster failed; retaining page for label rendering");
            }
        }
        if (result.ok && work->prepare_background) {
            auto& image = result.background;
            image.width = work->screen_width; image.height = work->height;
            image.pixels.resize(static_cast<size_t>(image.width) * image.height);
            for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x)
                image.pixels[y * image.width + x] = reader::ReadingBackgroundPixel(work->background, x, y,
                    work->background_left, work->width, work->height);
            image.BindDsc();
        }
    } catch (const std::bad_alloc&) {
        result.ok = false;
        ESP_LOGE(kTag, "page preparation allocation failed");
    }
    result.raster_us = esp_timer_get_time() - started - result.navigation_us;
    // 单个在途任务的固定交接槽；队列拥塞不会丢失完成通知或遗留 busy。
    auto* output = work->result.release();
    delete work;
    completed.store(output, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}
}

bool StartReaderPageWork(std::unique_ptr<ReaderPageResult> result) {
    auto* work = new(std::nothrow) Work{};
    if (!work) { ESP_LOGE(kTag, "page request allocation failed"); return false; }
    work->result = std::move(result);
    const auto& st = Book_State();
    work->font = BookFont();
    work->width = st.reader.viewport_w; work->height = st.reader.viewport_h;
    work->vertical = st.reader.session->VerticalWriting();
    work->rtl = BookReaderPrefsTextDirection() == kBookReaderTextRightToLeft;
    work->underline = BookReaderPrefsUnderlineMode(); work->antialias = BookReaderPrefsAntialias() != 0;
    work->background = st.reader.session->IsPageImagesMode() ? 0 : BookReaderPrefsBackground();
    work->screen_width = LV_HOR_RES;
    const auto area = ReadContentArea();
    work->background_left = area.x1;
    work->prepare_background = work->background && work->height > 0 &&
        (!st.reader.background_layer || st.reader.background_mode != work->background ||
         st.reader.background_left != work->background_left || st.reader.background_width != work->width ||
         st.reader.background_height != work->height || st.reader.background_screen_width != work->screen_width ||
         st.reader.background_top != area.y1);
    if (xTaskCreateWithCaps(PreparePage, "reader_page", 24 * 1024, work, 2, nullptr,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete work;
        ESP_LOGE(kTag, "page task creation failed");
        return false;
    }
    return true;
}

std::unique_ptr<ReaderPageResult> TakeCompletedReaderPageWork() {
    return std::unique_ptr<ReaderPageResult>(completed.exchange(nullptr, std::memory_order_acq_rel));
}
