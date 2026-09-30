#pragma GCC optimize("O1")

#include "book_screen/reader/book_page_image.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"
#include "screen_common.h"

struct PageImageDoneMsg {
    uint32_t token = 0;
    lv_obj_t* slot = nullptr;
    reader::RasterImage* image = nullptr;
    std::string href;
    bool ok = false;
    bool page_images = false;
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
    bool page_images = false;
    std::vector<PageImageWorkItem> items;
};
void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool page_images) {
    if (slot == nullptr || heap_img == nullptr || !lv_obj_is_valid(slot)) {
        delete heap_img;
        return;
    }
    lv_obj_clean(slot);
    heap_img->BindDsc();
    lv_obj_t* image = lv_image_create(slot);
    lv_image_set_src(image, &heap_img->dsc);
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
    if (page_images) {
        lv_obj_align(image, LV_ALIGN_TOP_LEFT, 0, 0);
    }
}

void AsyncPageImageDone(void* user_data) {
    auto* msg = static_cast<PageImageDoneMsg*>(user_data);
    auto& st = Book_State();
    const bool mine = (msg->token == st.reader.page_image_token.load());
    if (!mine || msg->slot == nullptr || !lv_obj_is_valid(msg->slot) || !msg->ok ||
        msg->image == nullptr || msg->image->empty()) {
        delete msg->image;
        delete msg;
        return;
    }
    if (st.reader.session && !msg->href.empty()) {
        reader::RasterImage cache_copy;
        cache_copy.pixels = msg->image->pixels;
        cache_copy.width = msg->image->width;
        cache_copy.height = msg->image->height;
        cache_copy.BindDsc();
        st.reader.session->SetCachedPageImage(msg->href, std::move(cache_copy));
    }
    AttachPageImageToSlot(msg->slot, msg->image, msg->page_images);
    msg->image = nullptr;
    delete msg;
}

void PageImageWorker(void* arg) {
    auto* ctx = static_cast<PageImageWorkerArg*>(arg);
    auto& st = Book_State();

    // EPUB：独立打开 ZIP，避免长时间占用 session 的 ZipReader 锁堵翻页
    std::unique_ptr<reader::EpubDocument> epub_doc;
    if (ctx->format == reader::BookFormat::kEpub && !ctx->book_path.empty()) {
        epub_doc = std::make_unique<reader::EpubDocument>();
        if (!epub_doc->Open(ctx->book_path.c_str())) {
            ESP_LOGW(TAG, "book_pimg epub open fail");
            epub_doc.reset();
        }
    }

    for (const auto& item : ctx->items) {
        if (ctx->token != st.reader.page_image_token.load()) {
            break;
        }
        auto* img = new reader::RasterImage();
        bool ok = false;
        if (epub_doc) {
            ok = epub_doc->DecodeItemImageToL8(item.href.c_str(), item.max_w, item.max_h, *img,
                                               &st.reader.page_image_abort) &&
                 !img->empty();
        } else if (st.reader.session) {
            ok = st.reader.session->LoadPageImage(item.href, item.max_w, item.max_h, *img) && !img->empty();
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
        done->page_images = ctx->page_images;
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
    if (epub_doc) {
        epub_doc->Close();
        epub_doc.reset();
    }

    auto* finish = new uint32_t(ctx->token);
    if (!ScreenLvAsync(
            [](void* p) {
                auto* tok = static_cast<uint32_t*>(p);
                auto& st = Book_State();
                st.reader.page_image_busy.store(false);
                st.reader.page_image_again = false;
                (void)tok;
                if (st.reader.deferred_cleanup && !ReaderWorkersBusy()) {
                    FinishDeferredCleanup();
                } else if (!st.reader.page_image_pending.empty()) {
                    StartPageImageWorker();
                }
                delete tok;
            },
            finish)) {
        st.reader.page_image_busy.store(false);
        delete finish;
    }
    delete ctx;
    vTaskDelete(nullptr);
}

void StartPageImageWorker() {
    auto& st = Book_State();
    if (st.reader.page_image_pending.empty() || !st.reader.session) {
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
    ctx->page_images = st.reader.session->IsPageImagesMode();
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

