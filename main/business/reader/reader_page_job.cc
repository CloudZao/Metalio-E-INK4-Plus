#include <algorithm>
#include <new>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "infra/tasks/task_create.h"
#include "infra/diagnostics/firmware_memory.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "infra/image/image_decode.h"
#include "business/reader/page_text_raster.h"
#include "business/reader/reader_page_render.h"
#include "business/wallpaper/wallpaper_active.h"
#include "reader_page_job.h"

static constexpr const char* kTag = "ReaderPageJob";
static std::atomic<bool> cancelled{false};
static std::atomic<ReaderPageStage> stage{ReaderPageStage::kIdle};
static std::atomic<bool> foreground_requested{false};

static void SetPreparationStage(ReaderPageStage next) {
    stage.store(next, std::memory_order_release);
    if (foreground_requested.exchange(false)) {
        vTaskPrioritySet(nullptr, tskIDLE_PRIORITY + 2);
    }
}

static void RenderPreparedPage(ReaderPageJob& work) {
    auto& result = *work.result;
    SetPreparationStage(ReaderPageStage::kBackground);
    if (result.ok && work.background && work.height > 0) {
        result.background = work.background_image;
        if (!result.background) {
            result.background = std::make_shared<RasterImage>();
            auto& image = *result.background;
            image.width = work.screen_width;
            image.height = work.screen_height;
            image.pixels.resize(static_cast<size_t>(image.width) * image.height, 255);
            if (work.background == 3) {
                char path[SD_FILE_PATH_CAPACITY] = {};
                RasterImage decoded;
                if (Wallpaper_TryResolveReadingWallpaperPath(path, sizeof(path)) &&
                    DecodeImageFileToL8(path, image.width, image.height, decoded,
                                        [](void*) { return cancelled.load(); })) {
                    const int left = (image.width - decoded.width) / 2;
                    const int top = (image.height - decoded.height) / 2;
                    for (int y = 0; y < decoded.height; ++y) {
                        if (cancelled.load(std::memory_order_relaxed)) {
                            break;
                        }
                        std::copy_n(decoded.pixels.data() + y * decoded.width, decoded.width,
                                    image.pixels.data() + (top + y) * image.width + left);
                    }
                } else if (!cancelled.load()) {
                    ESP_LOGW(kTag, "reading wallpaper unavailable; using white background");
                }
            } else {
                for (int y = 0; y < image.height; ++y) {
                    if (cancelled.load(std::memory_order_relaxed)) {
                        break;
                    }
                    for (int x = 0; x < image.width; ++x) {
                        const uint8_t pixel =
                            ReadingBackgroundPixel(work.background, x, y, image.width, image.height);
                        image.pixels[y * image.width + x] = result.dark ? 255 - pixel : pixel;
                    }
                }
            }
        }
        result.ok = !cancelled.load();
    }
    if (result.ok) {
        SetPreparationStage(ReaderPageStage::kFont);
        const auto* page = result.cursor->CurrentPageData();
        const bool warmed = !work.prewarm || work.prewarm(page, &cancelled);
        result.ok = !cancelled.load();
        const bool has_text = page && std::any_of(page->items.begin(), page->items.end(), [](const PageItem& item) {
                                  return item.kind == ContentKind::kText && !item.text.empty();
                              });
        if (warmed && result.ok && has_text && !result.cursor->IsPageImagesMode() && work.font &&
            work.width > 40 && work.height > 40) {
            result.column_width = work.vertical
                                      ? work.width
                                      : PageTextColumnWidth(*page, work.font, work.width, result.cursor->Style());
            SetPreparationStage(ReaderPageStage::kRaster);
            const bool raster_ok =
                work.vertical && PageIsTextOnly(*page)
                    ? RasterPageVerticalTextToL8(*page, work.font, result.cursor->LineSpacingPercent(),
                                                 result.cursor->ParaGap(), result.column_width, work.height,
                                                 work.underline, result.raster, work.antialias, work.rtl,
                                                 work.style, &cancelled)
                    : RasterPageTextToL8(*page, work.font, result.cursor->LineSpacingPercent(),
                                         result.cursor->ParaGap(), result.column_width, work.height,
                                         work.underline, result.raster, work.antialias, &result.image_tops,
                                         work.style, &cancelled);
            if (!raster_ok) {
                result.raster.Reset();
                result.image_tops.clear();
            }
            if (raster_ok) {
                result.ink_height = result.raster.height;
            }
            if (raster_ok && (result.background || result.dark)) {
                const bool dark = result.dark && work.background != 3;
                const int left = work.text_left + (work.width - result.column_width) / 2;
                for (int y = 0; y < result.raster.height; ++y) {
                    if (cancelled.load(std::memory_order_relaxed)) {
                        break;
                    }
                    for (int x = 0; x < result.raster.width; ++x) {
                        auto& pixel = result.raster.pixels[y * result.raster.width + x];
                        unsigned background = dark ? 0 : 255;
                        if (result.background && work.text_top + y < result.background->height && left + x >= 0 &&
                            left + x < result.background->width) {
                            background = result.background
                                             ->pixels[(work.text_top + y) * result.background->width + left + x];
                        }
                        pixel = dark ? 255 - ((255 - background) * pixel + 127) / 255
                                     : (background * pixel + 127) / 255;
                    }
                }
            }
            if (!raster_ok && !cancelled.load()) {
                ESP_LOGW(kTag, "text raster failed; retaining page for label rendering");
            }
        }
    }
}

static void PreparePage(void* data) {
    auto* work = static_cast<ReaderPageJob*>(data);
    auto& result = *work->result;
    const auto started = esp_timer_get_time();
    result.queue_us = started - result.started_us;
    try {
        const size_t frame = size_t(std::max(0, work->width)) * std::max(0, work->height);
        const size_t working = result.chapter_only ? frame : frame * 2;
        FirmwarePrefetchScope prefetch(working, result.prefetch);
        FirmwareMemoryDemand demand(working, !result.prefetch);
        result.reclaim_generation = FirmwareMemoryReclaimGeneration();
        SdAccessPermit permit;
        result.deferred = result.prefetch && !prefetch.Valid();
        result.ok = !result.deferred && !cancelled.load();
        if (result.ok && !result.navigation_ready) {
            result.ok = result.cursor->PrepareNavigation(result.kind, result.value, &cancelled, work->loading_chapter);
            result.navigation_ready = result.ok;
        }
        const auto navigated = esp_timer_get_time();
        result.navigation_us = navigated - started;
        if (result.ok && !result.chapter_only) {
            RenderPreparedPage(*work);
        }
        result.deferred = result.prefetch && !prefetch.Valid();
        result.ok = result.ok && !result.deferred;
    } catch (const std::bad_alloc&) {
        result.ok = false;
        ESP_LOGE(kTag, "page preparation allocation failed");
    }
    result.ok = result.ok && !cancelled.load();
    result.raster_us = esp_timer_get_time() - started - result.navigation_us;
    SetPreparationStage(ReaderPageStage::kFinishing);
    auto output = std::move(work->result);
    auto completed = work->completed;
    delete work;
    stage.store(ReaderPageStage::kIdle, std::memory_order_release);
    completed(std::move(output));
    vTaskDeleteWithCaps(nullptr);
}

bool ReaderPreparePage(std::unique_ptr<ReaderPageJob> work) {
    if (!work || !work->result || !work->result->cursor || !work->completed) {
        return false;
    }
    auto idle = ReaderPageStage::kIdle;
    if (!stage.compare_exchange_strong(idle, ReaderPageStage::kNavigation, std::memory_order_acq_rel)) {
        return false;
    }
    cancelled.store(false);
    foreground_requested.store(false);
    if (FirmwareCreateTask(PreparePage, "reader_page", 24 * 1024, work.get(), work->result->prefetch ? 1 : 2,
                            nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        stage.store(ReaderPageStage::kIdle);
        ESP_LOGW(kTag, "operation=prepare stage=create_task result=unavailable");
        return false;
    }
    work.release();
    return true;
}

ReaderPageStage ReaderPagePreparationStage() {
    return stage.load(std::memory_order_acquire);
}

void PromoteReaderPagePreparation() {
    foreground_requested.store(true);
}

void CancelReaderPagePreparation() {
    cancelled.store(true);
}
