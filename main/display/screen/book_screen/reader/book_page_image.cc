#include "epd_content_policy.h"
#include "assets/lang_config.h"
#pragma GCC optimize("O1")

#include "book_screen/reader/book_page_image.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_layout_debounce.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"
#include "sd_access.h"
#include "screen_common.h"

struct PageImageDoneMsg {
    uint32_t token = 0;
    lv_obj_t* slot = nullptr;
    reader::RasterImage* image = nullptr;
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

struct PageImageWorkerArg {
    uint32_t token = 0;
    std::string book_path;
    reader::BookFormat format = reader::BookFormat::kTxt;
    std::vector<PageImageWorkItem> items;
    reader::BookSession* session = nullptr; // busy 结束前由阅读生命周期保证存活
    int chapter = 0, radius = 0; // 原始文档章索引及单侧预取上限
    int frame_w = 0, frame_h = 0;
    std::string anchor_href; // 当前漫画页资源

};
void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool center) {
    if (slot == nullptr || heap_img == nullptr || !lv_obj_is_valid(slot)) {
        delete heap_img;
        return;
    }
    lv_obj_clean(slot);
    heap_img->BindDsc();
    lv_obj_t* image = lv_image_create(slot);
    lv_image_set_src(image, &heap_img->dsc);
    EpdSetContentRole(image,center ? EpdContentRole::Illustration : EpdContentRole::ReaderAntialias);
    lv_obj_set_user_data(image, heap_img);
    lv_obj_add_event_cb(
        image,
        [](lv_event_t* ev) {
            auto* p = static_cast<reader::RasterImage*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(ev))));
            delete p;
        },
        LV_EVENT_DELETE, nullptr);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(image);
    if (center) lv_obj_center(image);
}

void AsyncPageImageDone(void* user_data) {
    auto* msg = static_cast<PageImageDoneMsg*>(user_data);
    auto& st = Book_State();
    const bool mine = (msg->token == st.reader.page_image_token.load());
    if (!mine || msg->slot == nullptr || !lv_obj_is_valid(msg->slot) || !msg->ok ||
        msg->image == nullptr || msg->image->empty()) {
        if (mine && msg->slot && lv_obj_is_valid(msg->slot)) {
            lv_obj_clean(msg->slot);
            auto* label = lv_label_create(msg->slot);
            lv_label_set_text(label, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
            lv_obj_center(label);
        }
        delete msg->image;
        delete msg;
        return;
    }
    if (st.reader.session && !msg->href.empty()) {
        try {
            reader::RasterImage cache_copy;
            cache_copy.pixels = msg->image->pixels;
            cache_copy.width = msg->image->width;
            cache_copy.height = msg->image->height;
            cache_copy.BindDsc();
            st.reader.session->SetCachedPageImage(msg->href, msg->max_w, msg->max_h, std::move(cache_copy));
        } catch (const std::bad_alloc&) {
            // 缓存分配失败仍显示已解码的当前页。
        }
    }
    AttachPageImageToSlot(msg->slot, msg->image, true);
    msg->image = nullptr;
    delete msg;
}

namespace {
bool PageImageCancelled(const PageImageWorkerArg& ctx) {
    auto& st = Book_State();
    return st.reader.page_image_abort.load(std::memory_order_relaxed) ||
           ctx.token != st.reader.page_image_token.load();
}

bool DecodePageImage(const PageImageWorkItem& item, reader::EpubDocument* epub,
                     reader::EbookDocument* ebook, reader::RasterImage& image) {
    auto& abort = Book_State().reader.page_image_abort;
    if (epub) return epub->DecodeItemImageToL8(item.href.c_str(), item.max_w, item.max_h, image, &abort);
    if (!ebook) return false;
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
    return reader::EbookDocument::ParseImageHref(item.href, offset) &&
        ebook->LoadImageBlockBytes(offset, bytes) &&
        reader::DecodeImageToL8(bytes.data(), bytes.size(), item.max_w, item.max_h, image,
            [](void* context) { return static_cast<std::atomic<bool>*>(context)->load(); }, &abort);
}

std::vector<PageImageWorkItem> ImageNeighbors(const PageImageWorkerArg& ctx,
                                             reader::EpubDocument* epub, reader::EbookDocument* ebook) {
    std::vector<PageImageWorkItem> result;
    if (!ctx.radius || ctx.anchor_href.empty() || PageImageCancelled(ctx)) return result;
    const int count = epub ? epub->SpineCount() : ebook ? ebook->ChapterCount() : 0;
    if (!count || ctx.chapter < 0 || ctx.chapter >= count) return result;
    const auto read = [&](int chapter, const reader::ContentBlockSink& sink) {
        return epub ? epub->StreamChapterBlocks(chapter, sink) : ebook->StreamChapterBlocks(chapter, sink, true);
    };
    std::deque<std::string> before;
    std::vector<std::string> after;
    bool found = false, complete = false;
    const bool ok = read(ctx.chapter, [&](reader::ContentBlock&& block) {
        if (PageImageCancelled(ctx)) return false;
        if (block.kind != reader::ContentKind::kImage) return true;
        if (!found && block.image_href == ctx.anchor_href) { found = true; return true; }
        if (!found) {
            before.push_front(std::move(block.image_href));
            if (before.size() > size_t(ctx.radius)) before.pop_back();
        } else {
            after.push_back(std::move(block.image_href));
            if (after.size() == size_t(ctx.radius)) { complete = true; return false; }
        }
        return true;
    });
    if ((!ok && !complete) || !found || PageImageCancelled(ctx)) return result;
    for (int direction : {1, -1}) {
        for (int chapter = ctx.chapter + direction; chapter >= 0 && chapter < count; chapter += direction) {
            const size_t have = direction > 0 ? after.size() : before.size();
            if (have >= size_t(ctx.radius) || PageImageCancelled(ctx)) break;
            if (ebook && !ebook->ChapterLikelyHasContent(chapter)) continue;
            std::deque<std::string> images;
            bool enough = false;
            const bool loaded = read(chapter, [&](reader::ContentBlock&& block) {
                if (PageImageCancelled(ctx)) return false;
                if (block.kind != reader::ContentKind::kImage) return true;
                images.push_back(std::move(block.image_href));
                if (direction > 0 && have + images.size() >= size_t(ctx.radius)) {
                    enough = true; return false;
                }
                if (have + images.size() > size_t(ctx.radius)) images.pop_front();
                return true;
            });
            if ((!loaded && !enough) || PageImageCancelled(ctx)) break;
            if (direction > 0) for (auto& href : images) after.push_back(std::move(href));
            else for (auto it = images.rbegin(); it != images.rend(); ++it) before.push_back(std::move(*it));
        }
    }
    // 先照顾下一页和上一页，再向两侧扩展；已命中的窗口一起更新使用顺序。
    for (int i = 0; i < ctx.radius; ++i) {
        if (size_t(i) < after.size()) result.push_back({std::move(after[i]), ctx.frame_w, ctx.frame_h, nullptr});
        if (size_t(i) < before.size()) result.push_back({std::move(before[i]), ctx.frame_w, ctx.frame_h, nullptr});
    }
    for (const auto& item : result) ctx.session->HasCachedPageImage(item.href, item.max_w, item.max_h);
    return result;
}
} // namespace

void PageImageWorker(void* arg) {
    auto* ctx = static_cast<PageImageWorkerArg*>(arg);
    auto& st = Book_State();

    {
        sd_access::Permit permit;
        // 独立读取书籍资源，原图缓存仅在 LVGL 回调更新，避免重绘时并发读写。
        std::unique_ptr<reader::EpubDocument> epub_doc;
        std::unique_ptr<reader::EbookDocument> ebook_doc;
        if (ctx->format == reader::BookFormat::kEpub && !ctx->book_path.empty()) {
            epub_doc = std::make_unique<reader::EpubDocument>();
            if (!epub_doc->Open(ctx->book_path.c_str())) {
                ESP_LOGW(TAG, "book_pimg epub open fail");
                epub_doc.reset();
            }
        }
        if (ctx->format == reader::BookFormat::kEbook && !ctx->book_path.empty()) {
            ebook_doc = std::make_unique<reader::EbookDocument>();
            if (!ebook_doc->Open(ctx->book_path.c_str())) {
                ESP_LOGW(TAG, "book_pimg ebook open fail");
                ebook_doc.reset();
            }
        }

        for (const auto& item : ctx->items) {
            if (ctx->token != st.reader.page_image_token.load()) {
                break;
            }
            auto* img = new reader::RasterImage();
            bool ok = DecodePageImage(item, epub_doc.get(), ebook_doc.get(), *img) && !img->empty();
            if (!ok && ctx->radius && !PageImageCancelled(*ctx)) {
                ctx->session->ClearPageImageCache();
                delete img;
                img = new reader::RasterImage();
                ok = DecodePageImage(item, epub_doc.get(), ebook_doc.get(), *img) && !img->empty();
            }
            if (st.reader.page_image_abort.load(std::memory_order_relaxed) ||
                ctx->token != st.reader.page_image_token.load()) {
                delete img;
                break;
            }
            auto* done = new PageImageDoneMsg{};
            done->token = ctx->token;
            done->slot = item.slot;
            done->href = item.href;
            done->ok = ok;
            done->max_w = item.max_w; done->max_h = item.max_h;
            if (ok) {
                done->image = img;
            } else {
                delete img;
                done->image = nullptr;
            }
            if (!ScreenLvAsync(AsyncPageImageDone, done)) {
                delete done->image;
                delete done;
            }
        }
        for (const auto& item : ImageNeighbors(*ctx, epub_doc.get(), ebook_doc.get())) {
            if (PageImageCancelled(*ctx) || ctx->session->PageImagePrefetchRadius() == 0) break;
            if (ctx->session->HasCachedPageImage(item.href, item.max_w, item.max_h)) continue;
            reader::RasterImage image;
            if (!DecodePageImage(item, epub_doc.get(), ebook_doc.get(), image) || image.empty()) break;
            if (PageImageCancelled(*ctx)) break;
            ctx->session->SetCachedPageImage(item.href, item.max_w, item.max_h, std::move(image));
            vTaskDelay(1);
        }
        if (epub_doc) {
            epub_doc->Close();
            epub_doc.reset();
        }

    }
    auto* finish = new uint32_t(ctx->token);
    if (!ScreenLvAsync(
            [](void* p) {
                auto* tok = static_cast<uint32_t*>(p);
                auto& st = Book_State();
                st.reader.page_image_busy.store(false);
                const bool again = st.reader.page_image_again;
                st.reader.page_image_again = false;
                if (st.reader.standby_paused) { delete tok; return; }
                if (st.reader.deferred_cleanup && !ReaderWorkersBusy()) {
                    FinishDeferredCleanup();
                } else if (!st.reader.layout_again && (!st.reader.page_image_pending.empty() || again)) {
                    StartPageImageWorker();
                }
                delete tok;
            },
            finish)) {
        st.reader.page_image_busy.store(false);
        delete finish;
    }
    delete ctx;
    vTaskDeleteWithCaps(nullptr);
}

void StartPageImageWorker() {
    if (ReaderPageWorkPending()) return;
    auto& st = Book_State();
    if (st.reader.standby_paused) return;
    if (!st.reader.session) return;
    const bool prefetch = st.reader.session->IsPageImagesMode() &&
        st.reader.read_chrome == BookUiState::ReadChrome::kReading;
    if (st.reader.page_image_pending.empty() && !prefetch) return;
    if (st.reader.opening.load() || st.reader.layout_busy.load() ||
        st.reader.session->LiveRelayoutPending()) return;
    if (st.reader.chapter_pages_busy.load()) {
        st.reader.session->AbortChapterPageTable();
        return;
    }
    if (st.reader.page_image_busy.load()) {
        st.reader.page_image_again = true;
        return;
    }
    const uint32_t token = st.reader.page_image_token.load();
    auto* ctx = new PageImageWorkerArg{};
    ctx->token = token;
    ctx->book_path = st.reader.session->Info().path;
    ctx->format = st.reader.session->Info().format;
    ctx->session = st.reader.session.get();
    if (prefetch) {
        ctx->radius = ctx->session->PageImagePrefetchRadius();
        ctx->frame_w = ctx->session->ViewportWidth(); ctx->frame_h = ctx->session->ViewportHeight();
        reader::BookSourcePosition first, end;
        if (ctx->session->CaptureBookmarkRange(first, end)) ctx->chapter = first.chapter;
        else ctx->radius = 0;
        if (const auto* page = ctx->session->CurrentPageData()) {
            for (const auto& item : page->items) if (item.kind == reader::ContentKind::kImage) {
                ctx->anchor_href = item.image_href; break;
            }
        }
    }
    ctx->items.reserve(st.reader.page_image_pending.size());
    for (auto& p : st.reader.page_image_pending) {
        PageImageWorkItem item;
        item.href = std::move(p.href);
        item.max_w = p.max_w;
        item.max_h = p.max_h;
        item.slot = p.slot;
        ctx->items.push_back(std::move(item));
    }
    st.reader.page_image_pending.clear();
    if (ctx->items.empty() && !ctx->radius) { delete ctx; return; }
    st.reader.page_image_abort.store(false, std::memory_order_relaxed);
    st.reader.page_image_busy.store(true);
    st.reader.page_image_again = false;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        PageImageWorker, "book_pimg", kPageImageWorkerStack, ctx, tskIDLE_PRIORITY + 1, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "book_pimg task failed");
        st.reader.page_image_busy.store(false);
        delete ctx;
    }
}

