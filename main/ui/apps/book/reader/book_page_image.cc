#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "infra/tasks/task_create.h"
#include "infra/diagnostics/firmware_memory.h"
#include "assets/lang_config.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/book/reader/book_page_image.h"
#include "boards/common/storage/sd_access.h"
#include "business/reader/reader.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr size_t kImageNeighborBatch = 8; // 单次资源名解析窗口，不限制像素缓存总张数

struct PageImageDoneMsg : ScreenUiCompletion {
    uint32_t token = 0;
    uint32_t reclaim_generation = 0; // 解码开始时的回收代次
    lv_obj_t* slot = nullptr;
    std::unique_ptr<RasterImage> image;
    std::string href;
    bool ok = false;
    int max_w = 0, max_h = 0;
};

struct PageImageWorkItem {
    std::string href;
    int max_w = 0;
    int max_h = 0;
    lv_obj_t* slot = nullptr;
};

struct PageImageWorkerArg : ScreenUiCompletion {
    uint32_t token = 0;
    std::string book_path;
    BookFormat format = BookFormat::kTxt;
    std::vector<PageImageWorkItem> items;
    BookSession* session = nullptr;  // busy 结束前由阅读生命周期保证存活
    int direction = 1; // 工作任务持有的阅读方向快照
    int chapter = 0; // 原始文档章索引
    bool prefetch = false; // 当前阅读位置是否需要邻图
    int frame_w = 0, frame_h = 0;
    std::string anchor_href;  // 当前漫画页资源
};

struct PageImageNeighborCursor {
    int chapter = 0; // 当前源章节
    std::string anchor; // 首次定位的当前图片
    size_t anchor_index = SIZE_MAX; // 后续批次按图片序号定位，允许资源名重复
    size_t first_index = 0; // 窗口首项在章节中的图片序号
    std::vector<std::string> images; // 仅保留有界资源名窗口
    size_t next = 0; // 当前窗口已经交付的项数
    bool chapter_complete = false; // 窗口耗尽后是否可以直接进入相邻章节
};

static int s_prefetch_direction = 1; // 仅 UI 任务更新

static void ShowPageImageFailure(lv_obj_t* slot) {
    if (slot == nullptr || !lv_obj_is_valid(slot)) {
        return;
    }
    lv_obj_clean(slot);
    auto* label = lv_label_create(slot);
    lv_label_set_text(label, LangString_BOOK_PAGE_LOAD_FAILED);
    lv_obj_center(label);
}

static void FailPendingPageImages() {
    auto& pending = Book_State().reader.page_image_pending;
    for (const auto& item : pending) {
        ShowPageImageFailure(item.slot);
    }
    pending.clear();
}

static bool PageImageCancelled(const PageImageWorkerArg& ctx) {
    auto& st = Book_State();
    return st.reader.page_image_abort.load(std::memory_order_relaxed) || ctx.token != st.reader.page_image_token.load();
}

static bool DecodePageImage(const PageImageWorkItem& item, EpubDocument* epub, EbookDocument* ebook,
                            RasterImage& image) {
    auto& abort = Book_State().reader.page_image_abort;
    if (epub) {
        return epub->DecodeItemImageToL8(item.href.c_str(), item.max_w, item.max_h, image, &abort);
    }
    if (!ebook) {
        return false;
    }
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
    return EbookDocument::ParseImageHref(item.href, offset) && ebook->LoadImageBlockBytes(offset, bytes) &&
           DecodeImageToL8(
               bytes.data(), bytes.size(), item.max_w, item.max_h, image,
               [](void* context) { return static_cast<std::atomic<bool>*>(context)->load(); }, &abort);
}

static bool NextImageNeighbor(const PageImageWorkerArg& ctx, EpubDocument* epub, EbookDocument* ebook,
                              int direction, PageImageNeighborCursor& cursor, PageImageWorkItem& item) {
    const int count = epub ? epub->SpineCount() : ebook ? ebook->ChapterCount() : 0;
    while (cursor.chapter >= 0 && cursor.chapter < count) {
        if (PageImageCancelled(ctx) || !FirmwarePrefetchAllowed()) {
            return false;
        }
        if (cursor.next < cursor.images.size()) {
            const size_t index = direction > 0 ? cursor.next : cursor.images.size() - 1 - cursor.next;
            ++cursor.next;
            cursor.anchor_index = cursor.first_index + index;
            item = {std::move(cursor.images[index]), ctx.frame_w, ctx.frame_h, nullptr};
            return true;
        }
        if (cursor.chapter_complete) {
            cursor.chapter += direction;
            cursor.anchor.clear();
            cursor.anchor_index = SIZE_MAX;
            cursor.chapter_complete = false;
            continue;
        }
        cursor.images.clear();
        cursor.next = 0;
        bool found = cursor.anchor.empty() && cursor.anchor_index == SIZE_MAX;
        bool stopped = false, more_before = false;
        size_t image_index = 0;
        const auto sink = [&](ContentBlock&& block) {
            if (PageImageCancelled(ctx) || !FirmwarePrefetchAllowed()) {
                return false;
            }
            if (block.kind != ContentKind::kImage) {
                return true;
            }
            const size_t index = image_index++;
            const bool anchor = cursor.anchor_index != SIZE_MAX ? index == cursor.anchor_index
                                                                : block.image_href == cursor.anchor;
            if (direction > 0 && !found) {
                found = anchor;
                return true;
            }
            if (direction < 0 && anchor) {
                found = true;
                stopped = true;
                return false;
            }
            if (cursor.images.size() == kImageNeighborBatch) {
                cursor.images.erase(cursor.images.begin());
                ++cursor.first_index;
                more_before = true;
            }
            if (cursor.images.empty()) {
                cursor.first_index = index;
            }
            cursor.images.push_back(std::move(block.image_href));
            if (direction > 0 && cursor.images.size() == kImageNeighborBatch) {
                stopped = true;
                return false;
            }
            return true;
        };
        bool loaded = false;
        {
            SdAccessPermit permit;
            loaded = epub ? epub->StreamChapterBlocks(cursor.chapter, sink)
                          : ebook->StreamChapterBlocks(cursor.chapter, sink, true);
        }
        if ((!loaded && !stopped) || !found || PageImageCancelled(ctx) || !FirmwarePrefetchAllowed()) {
            return false;
        }
        cursor.chapter_complete = direction > 0 ? !stopped : !more_before;
        if (cursor.images.empty()) {
            cursor.chapter_complete = true;
        }
    }
    return false;
}

static void AsyncPageImageDone(void* user_data) {
    std::unique_ptr<PageImageDoneMsg> msg(static_cast<PageImageDoneMsg*>(user_data));
    auto& st = Book_State();
    const bool mine = (msg->token == st.reader.page_image_token.load());
    if (!mine || msg->slot == nullptr || !lv_obj_is_valid(msg->slot) || !msg->ok || msg->image == nullptr ||
        msg->image->empty()) {
        if (mine) {
            ShowPageImageFailure(msg->slot);
        }
        return;
    }
    if (st.reader.session && !msg->href.empty() &&
        msg->reclaim_generation == FirmwareMemoryReclaimGeneration()) {
        try {
            FirmwarePrefetchScope cache(msg->image->pixels.size());
            if (cache.Valid()) {
                RasterImage cache_copy(*msg->image);
                st.reader.session->SetCachedPageImage(msg->href, msg->max_w, msg->max_h, std::move(cache_copy),
                                                     msg->reclaim_generation);
            }
        } catch (const std::bad_alloc&) {
            ESP_LOGD(TAG, "operation=page_image stage=cache_copy result=no_memory");
        }
    }
    AttachPageImageToSlot(msg->slot, msg->image.release(), true);
}

static void PageImageWorker(void* arg) {
    auto* ctx = static_cast<PageImageWorkerArg*>(arg);
    auto& st = Book_State();

    try {
        // 独立读取资源；当前页交给 UI，邻图由会话缓存锁保护。
        std::unique_ptr<EpubDocument> epub_doc;
        std::unique_ptr<EbookDocument> ebook_doc;
        {
            const bool optional = ctx->items.empty();
            FirmwarePrefetchScope setup(kFirmwarePsramReserveBytes, optional);
            FirmwareMemoryDemand demand(kFirmwarePsramReserveBytes, !optional);
            if (!optional || setup.Valid()) {
                if (ctx->format == BookFormat::kEpub && !ctx->book_path.empty()) {
                    epub_doc = std::make_unique<EpubDocument>();
                    SdAccessPermit permit;
                    if (!epub_doc->Open(ctx->book_path.c_str(), false)) {
                        ESP_LOGW(TAG, "book_pimg epub open fail");
                        epub_doc.reset();
                    }
                }
                if (ctx->format == BookFormat::kEbook && !ctx->book_path.empty()) {
                    ebook_doc = std::make_unique<EbookDocument>();
                    SdAccessPermit permit;
                    if (!ebook_doc->Open(ctx->book_path.c_str())) {
                        ESP_LOGW(TAG, "book_pimg ebook open fail");
                        ebook_doc.reset();
                    }
                }
            } else {
                ctx->prefetch = false;
            }
        }

        for (const auto& item : ctx->items) {
            if (ctx->token != st.reader.page_image_token.load()) {
                break;
            }
            const uint32_t reclaim_generation = FirmwareMemoryReclaimGeneration();
            FirmwareMemoryDemand demand(size_t(std::max(0, item.max_w)) * std::max(0, item.max_h) +
                                        kFirmwarePsramReserveBytes);
            auto img = std::make_unique<RasterImage>();
            bool ok = false;
            {
                SdAccessPermit permit;
                ok = DecodePageImage(item, epub_doc.get(), ebook_doc.get(), *img) && !img->empty();
            }
            if (st.reader.page_image_abort.load(std::memory_order_relaxed) ||
                ctx->token != st.reader.page_image_token.load()) {
                break;
            }
            auto done = std::make_unique<PageImageDoneMsg>();
            done->token = ctx->token;
            done->reclaim_generation = reclaim_generation;
            done->slot = item.slot;
            done->href = item.href;
            done->ok = ok;
            done->max_w = item.max_w;
            done->max_h = item.max_h;
            if (ok) {
                done->image = std::move(img);
            }
            done->apply = [](ScreenUiCompletion* completion) { AsyncPageImageDone(completion); };
            ScreenCompleteWork(done.release());
        }
        if (ctx->prefetch) {
            ctx->session->PreparePageImagePrefetch(ctx->anchor_href);
        }
        PageImageNeighborCursor neighbors[2];
        for (auto& cursor : neighbors) {
            cursor.chapter = ctx->chapter;
            cursor.anchor = ctx->anchor_href;
        }
        bool exhausted[2] = {!ctx->prefetch, !ctx->prefetch};
        const size_t frame = size_t(std::max(0, ctx->frame_w)) * std::max(0, ctx->frame_h);
        while (!exhausted[0] || !exhausted[1]) {
            if (PageImageCancelled(*ctx) || !FirmwarePrefetchAllowed()) {
                break;
            }
            bool admitted = false;
            for (int turn = 0; turn < 2; ++turn) {
                const int side = ctx->direction < 0 ? 1 - turn : turn;
                if (exhausted[side]) {
                    continue;
                }
                FirmwarePrefetchScope prefetch(frame + kFirmwarePsramReserveBytes);
                if (!prefetch.Valid()) {
                    break;
                }
                admitted = true;
                const uint32_t reclaim_generation = FirmwareMemoryReclaimGeneration();
                PageImageWorkItem item;
                if (!NextImageNeighbor(*ctx, epub_doc.get(), ebook_doc.get(), side == 0 ? 1 : -1,
                                       neighbors[side], item)) {
                    exhausted[side] = true;
                    continue;
                }
                if (ctx->session->HasCachedPageImage(item.href, item.max_w, item.max_h)) {
                    continue;
                }
                if (!ctx->session->CanPrefetchPageImage()) {
                    exhausted[0] = exhausted[1] = true;
                    break;
                }
                RasterImage image;
                bool decoded = false;
                {
                    SdAccessPermit permit;
                    decoded = DecodePageImage(item, epub_doc.get(), ebook_doc.get(), image);
                }
                if (!decoded || image.empty() || PageImageCancelled(*ctx) || !prefetch.Valid()) {
                    exhausted[0] = exhausted[1] = true;
                    break;
                }
                ctx->session->SetCachedPageImage(item.href, item.max_w, item.max_h, std::move(image),
                                                 reclaim_generation);
            }
            if (!admitted) {
                break;
            }
            vTaskDelay(1);
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=page_image stage=execute result=no_memory");
    }
    ctx->apply = [](ScreenUiCompletion* completion) {
        std::unique_ptr<PageImageWorkerArg> owner(static_cast<PageImageWorkerArg*>(completion));
        auto& state = Book_State();
        state.reader.page_image_busy.store(false);
        const bool again = state.reader.page_image_again;
        state.reader.page_image_again = false;
        if (!state.reader.standby_paused && !state.reader.deferred_cleanup &&
            !state.reader.layout_again && (!state.reader.page_image_pending.empty() || again)) {
            StartPageImageWorker();
        }
        BookRequestUiWork();
    };
    ScreenCompleteWork(ctx);
    vTaskDeleteWithCaps(nullptr);
}

void AttachPageImageToSlot(lv_obj_t* slot, RasterImage* heap_img, bool center) {
    if (slot == nullptr || heap_img == nullptr || !lv_obj_is_valid(slot)) {
        delete heap_img;
        return;
    }
    lv_obj_clean(slot);
    heap_img->BindDsc();
    lv_obj_t* image = lv_image_create(slot);
    lv_image_set_src(image, &heap_img->dsc);
    EpdSetContentRole(image, center ? EpdContentRole::Illustration : EpdContentRole::ReaderAntialias);
    lv_obj_set_user_data(image, heap_img);
    lv_obj_add_event_cb(
        image,
        [](lv_event_t* ev) {
            auto* p = static_cast<RasterImage*>(lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(ev))));
            delete p;
        },
        LV_EVENT_DELETE, nullptr);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(image);
    if (center) {
        lv_obj_center(image);
    }
}

void StartPageImageWorker(int direction) try {
    if (direction) {
        s_prefetch_direction = direction < 0 ? -1 : 1;
    }
    if (ReaderPageWorkPending()) {
        return;
    }
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        return;
    }
    if (!st.reader.session || !st.reader.session->Style().inline_images) {
        return;
    }
    const bool prefetch =
        st.reader.session->IsPageImagesMode() && st.reader.read_chrome == BookUiState::ReadChrome::kReading;
    if (st.reader.page_image_pending.empty() && !prefetch) {
        return;
    }
    const bool chapter_relayout = st.reader.session->LiveRelayoutPending() && !st.reader.session->StructurePending();
    if (st.reader.opening.load() || st.reader.layout_busy.load() || chapter_relayout) {
        return;
    }
    if (st.reader.page_image_busy.load()) {
        st.reader.page_image_again = true;
        return;
    }
    const uint32_t token = st.reader.page_image_token.load();
    auto ctx = std::make_unique<PageImageWorkerArg>();
    ctx->token = token;
    ctx->direction = s_prefetch_direction;
    ctx->book_path = st.reader.session->Info().path;
    ctx->format = st.reader.session->Info().format;
    ctx->session = st.reader.session.get();
    if (prefetch) {
        ctx->prefetch = FirmwarePrefetchAllowed();
        ctx->frame_w = ctx->session->ViewportWidth();
        ctx->frame_h = ctx->session->ViewportHeight();
        BookSourcePosition first, end;
        if (ctx->session->CaptureBookmarkRange(first, end)) {
            ctx->chapter = first.chapter;
        } else {
            ctx->prefetch = false;
        }
        if (const auto* page = ctx->session->CurrentPageData()) {
            for (const auto& item : page->items) {
                if (item.kind == ContentKind::kImage) {
                    ctx->anchor_href = item.image_href;
                    break;
                }
            }
        }
    }
    ctx->prefetch = ctx->prefetch && !ctx->anchor_href.empty();
    ctx->items.reserve(st.reader.page_image_pending.size());
    for (const auto& p : st.reader.page_image_pending) {
        PageImageWorkItem item;
        item.href = p.href;
        item.max_w = p.max_w;
        item.max_h = p.max_h;
        item.slot = p.slot;
        ctx->items.push_back(std::move(item));
    }
    if (ctx->items.empty() && !ctx->prefetch) {
        return;
    }
    st.reader.page_image_abort.store(false, std::memory_order_relaxed);
    st.reader.page_image_busy.store(true);
    st.reader.page_image_again = false;
    BaseType_t created =
        FirmwareCreateTask(PageImageWorker, "book_pimg", kPageImageWorkerStack, ctx.get(), tskIDLE_PRIORITY + 1,
                                        nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "operation=page_image stage=create_task result=unavailable; show failed slots");
        st.reader.page_image_busy.store(false);
        FailPendingPageImages();
    } else {
        st.reader.page_image_pending.clear();
        ctx.release();
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=page_image stage=submit result=no_memory");
    FailPendingPageImages();
}
