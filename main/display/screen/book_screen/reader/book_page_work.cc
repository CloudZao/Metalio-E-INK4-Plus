#include "book_screen/book_geom.h"
#include "book_page_work.h"

#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "reader/page_text_raster.h"
#include "reader/reader_page_render.h"
#include "reader/image_util.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "sd_paths.h"
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
std::atomic<bool> cancelled{false};

struct Work {
    std::unique_ptr<ReaderPageResult> result; // 完成后转交 UI 的结果
    const lv_font_t* font = nullptr; // 工作期间保持存活的正文字体
    int width = 0, height = 0, underline = 0; // 分页视口和下划线模式
    int background = 0, text_left = 0, text_top = 0; // 背景模式与正文屏幕坐标
    int screen_width = 0, screen_height = 0; // 整页背景尺寸
    std::shared_ptr<reader::RasterImage> background_image; // 页面准备期间保持背景存活
    bool antialias = false, vertical = false, rtl = false; // 抗锯齿、竖排和列方向
};

void PreparePage(void* data) {
    auto* work = static_cast<Work*>(data);
    auto& result = *work->result;
    const auto started = esp_timer_get_time();
    result.queue_us = started - result.started_us;
    try {
        sd_access::Permit permit;
        result.ok = !cancelled.load() && result.cursor->PrepareNavigation(result.kind, result.value, &cancelled);
        const auto navigated = esp_timer_get_time();
        result.navigation_us = navigated - started;
        if (result.ok && work->background && work->height > 0) {
            result.background = work->background_image;
            if (!result.background) {
                result.background = std::make_shared<reader::RasterImage>();
                auto& image = *result.background;
                image.width = work->screen_width; image.height = work->screen_height;
                image.pixels.resize(static_cast<size_t>(image.width) * image.height, 255);
                if (work->background == 3) {
                    char path[SD_FILE_PATH_CAPACITY] = {};
                    reader::RasterImage decoded;
                    if (Wallpaper_TryResolveReadingWallpaperPath(path, sizeof(path)) &&
                        reader::DecodeImageFileToL8(path, image.width, image.height, decoded,
                            [](void*) { return cancelled.load(); })) {
                        const int left = (image.width - decoded.width) / 2;
                        const int top = (image.height - decoded.height) / 2;
                        for (int y = 0; y < decoded.height; ++y)
                            std::copy_n(decoded.pixels.data() + y * decoded.width, decoded.width,
                                image.pixels.data() + (top + y) * image.width + left);
                    } else if (!cancelled.load()) {
                        ESP_LOGW(kTag, "reading wallpaper unavailable; using white background");
                    }
                } else {
                    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x)
                        image.pixels[y * image.width + x] = reader::ReadingBackgroundPixel(work->background, x, y,
                            image.width, image.height);
                }
            }
            result.ok = !cancelled.load();
        }
        if (result.ok) {
            const auto* page = result.cursor->CurrentPageData();
            const bool warmed = PrewarmPageFont(page, &cancelled);
            result.ok = !cancelled.load();
            if (warmed && result.ok && page && !result.cursor->IsPageImagesMode() && reader::PageIsTextOnly(*page) &&
                work->font && work->width > 40 && work->height > 40) {
                result.column_width = work->vertical ? work->width
                    : reader::PageTextColumnWidth(*page, work->font, work->width);
                const bool raster_ok = work->vertical
                    ? reader::RasterPageVerticalTextToL8(*page, work->font, result.cursor->LineSpacingPercent(), result.cursor->ParaGap(),
                        result.column_width, work->height, work->underline, result.raster, work->antialias, work->rtl)
                    : reader::RasterPageTextToL8(*page, work->font, result.cursor->LineSpacingPercent(), result.cursor->ParaGap(),
                        result.column_width, work->height, work->underline, result.raster, work->antialias);
                if (raster_ok) result.ink_height = result.raster.height;
                if (raster_ok && result.background) {
                    const auto& background = *result.background;
                    const int left = work->text_left + (work->width - result.column_width) / 2;
                    for (int y = 0; y < result.raster.height; ++y) for (int x = 0; x < result.raster.width; ++x) {
                        auto& pixel = result.raster.pixels[y * result.raster.width + x];
                        if (work->text_top + y < background.height && left + x >= 0 && left + x < background.width)
                            pixel = static_cast<uint8_t>((static_cast<unsigned>(pixel) *
                                background.pixels[(work->text_top + y) * background.width + left + x] + 127) / 255);
                    }
                }
                if (!raster_ok) ESP_LOGW(kTag, "text raster failed; retaining page for label rendering");
            }
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
    cancelled.store(false);
    const auto& st = Book_State();
    work->font = BookFont();
    work->width = st.reader.viewport_w; work->height = st.reader.viewport_h;
    work->vertical = st.reader.session->VerticalWriting();
    work->rtl = BookReaderPrefsTextDirection() == kBookReaderTextRightToLeft;
    work->underline = BookReaderPrefsUnderlineMode(); work->antialias = BookReaderPrefsAntialias() != 0;
    work->background = st.reader.session->IsPageImagesMode() ? 0 : BookReaderPrefsBackground();
    const auto background_area = ReadBackgroundArea();
    work->screen_width = lv_area_get_width(&background_area);
    work->screen_height = lv_area_get_height(&background_area);
    const auto area = ReadContentArea();
    work->text_left = area.x1;
    work->text_top = area.y1;
    if (work->background && st.reader.background_mode == work->background &&
        st.reader.background_image && st.reader.background_image->width == work->screen_width &&
        st.reader.background_image->height == work->screen_height) work->background_image = st.reader.background_image;
    if (xTaskCreateWithCaps(PreparePage, "reader_page", 24 * 1024, work, work->result->prefetch ? 1 : 2, nullptr,
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

void CancelReaderPagePreparation() { cancelled.store(true); }
