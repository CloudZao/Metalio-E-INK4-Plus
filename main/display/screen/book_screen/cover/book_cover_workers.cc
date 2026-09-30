#pragma GCC optimize("O1")

#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/cover/book_cover_loader.h"
#include "book_screen/cover/book_cover_style.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_body.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "screen_common.h"

struct DetailCoverDoneMsg {
    uint32_t token = 0;
    bool cover_ok = false;
    reader::RasterImage* cover = nullptr;
    std::string title;
    std::string author;
    std::string path;
};

struct DetailCoverWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;
    int max_w = 0;
    int max_h = 0;
};
struct ListCoverDoneMsg {
    uint32_t token = 0;
    lv_obj_t* host = nullptr;
    reader::RasterImage* slot = nullptr;
    reader::RasterImage* cover = nullptr;
    std::string path;
    bool cover_ok = false;
};

struct ListCoverFillWork {
    uint32_t token = 0;
    std::vector<BookUiState::ListCoverPending> items;
};

struct CoverMissMsg {
    std::string path;
};
bool IsCoverEmbedMiss(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const auto& miss = Book_State().shelf.cover_embed_miss;
    return std::find(miss.begin(), miss.end(), path) != miss.end();
}

void RememberCoverEmbedMiss(const std::string& path) {
    if (path.empty() || IsCoverEmbedMiss(path)) {
        return;
    }
    Book_State().shelf.cover_embed_miss.push_back(path);
}

void AsyncRememberCoverMiss(void* user_data) {
    auto* msg = static_cast<CoverMissMsg*>(user_data);
    RememberCoverEmbedMiss(msg->path);
    delete msg;
}

void PostCoverEmbedMiss(const std::string& path) {
    if (path.empty()) {
        return;
    }
    auto* msg = new CoverMissMsg{};
    msg->path = path;
    if (!ScreenLvAsync(AsyncRememberCoverMiss, msg)) {
        delete msg;
    }
}

void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot) {
    if (host == nullptr || slot == nullptr || max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return;
    }
    if (info.format != reader::BookFormat::kEpub && info.format != reader::BookFormat::kEbook) {
        return;
    }
    if (IsCoverEmbedMiss(info.path)) {
        return;
    }
    auto& st = Book_State();
    BookUiState::ListCoverPending p;
    p.info = info;
    p.max_w = max_w;
    p.max_h = max_h;
    p.host = host;
    p.slot = slot;
    st.shelf.list_cover_pending.push_back(std::move(p));
}

void ApplyListCoverImage(lv_obj_t* host, reader::RasterImage& cover) {
    if (host == nullptr || !lv_obj_is_valid(host) || cover.empty()) {
        return;
    }
    cover.BindDsc();
    lv_obj_clean(host);
    lv_obj_t* img = lv_image_create(host);
    lv_image_set_src(img, &cover.dsc);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void AsyncListCoverDone(void* user_data) {
    auto* msg = static_cast<ListCoverDoneMsg*>(user_data);
    auto& st = Book_State();
    if (!msg->cover_ok) {
        RememberCoverEmbedMiss(msg->path);
    }
    if (msg->token != st.shelf.list_cover_token.load() || msg->cover == nullptr) {
        delete msg->cover;
        delete msg;
        return;
    }
    if (msg->host == nullptr || !lv_obj_is_valid(msg->host) || msg->slot == nullptr) {
        delete msg->cover;
        delete msg;
        return;
    }
    if (msg->cover_ok && !msg->cover->empty()) {
        *msg->slot = std::move(*msg->cover);
        ApplyListCoverImage(msg->host, *msg->slot);
    }
    delete msg->cover;
    delete msg;
}

void ListCoverFillWorker(void* arg) {
    auto* work = static_cast<ListCoverFillWork*>(arg);
    auto& st = Book_State();
    for (size_t i = 0; i < work->items.size(); ++i) {
        if (work->token != st.shelf.list_cover_token.load()) {
            break;
        }
        const BookUiState::ListCoverPending& item = work->items[i];
        auto* cover = new reader::RasterImage();
        reader::BookInfo info = item.info;
        const bool ok = BookCoverLoader_LoadBookDetailCover(info, item.max_w, item.max_h, *cover) &&
                        !cover->empty();
        auto* done = new ListCoverDoneMsg{};
        done->token = work->token;
        done->host = item.host;
        done->slot = item.slot;
        done->path = item.info.path;
        done->cover_ok = ok;
        if (ok) {
            done->cover = cover;
        } else {
            delete cover;
            done->cover = nullptr;
        }
        if (!ScreenLvAsync(AsyncListCoverDone, done)) {
            if (!ok) {
                PostCoverEmbedMiss(item.info.path);
            }
            delete done->cover;
            delete done;
        }
        // 串行抽封面，让出 CPU，避免列表滑动/点击发闷
        vTaskDelay(pdMS_TO_TICKS(ok ? 15 : 5));
    }
    delete work;
    vTaskDelete(nullptr);
}

void ScheduleListCoverFill() {
    auto& st = Book_State();
    if (st.shelf.list_cover_pending.empty()) {
        return;
    }
    ESP_LOGI(TAG, "list_cover fill n=%u", static_cast<unsigned>(st.shelf.list_cover_pending.size()));
    auto* work = new ListCoverFillWork{};
    work->token = st.shelf.list_cover_token.load();
    work->items = std::move(st.shelf.list_cover_pending);
    st.shelf.list_cover_pending.clear();
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        ListCoverFillWorker, "list_cover", kListCoverWorkerStack, work, tskIDLE_PRIORITY + 1,
        nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "list_cover task failed; keep placeholders");
        delete work;
    }
}

void FillDetailCoverPlaceholder(lv_obj_t* host, reader::BookFormat format) {
    auto& st = Book_State();
    if (host == nullptr) {
        return;
    }
    lv_obj_clean(host);
    lv_obj_t* ph = lv_obj_create(host);
    lv_obj_remove_style_all(ph);
    lv_obj_set_size(ph, st.detail.detail_cover_fw, st.detail.detail_cover_fh);
    lv_obj_set_style_pad_all(ph, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    StyleBookCoverFrame(ph);
    lv_obj_t* ph_lbl = lv_label_create(ph);
    lv_label_set_text(ph_lbl, reader::FormatLabel(format));
    lv_obj_set_style_text_font(ph_lbl, Book_ListFont(), 0);
    lv_obj_center(ph_lbl);
    Book_DisableScroll(ph);
}

void ApplyDetailCoverImage() {
    auto& st = Book_State();
    if (st.detail.detail_cover_host == nullptr || !lv_obj_is_valid(st.detail.detail_cover_host) ||
        st.detail.detail_cover.empty()) {
        return;
    }
    st.detail.detail_cover.BindDsc();
    lv_obj_clean(st.detail.detail_cover_host);
    lv_obj_t* frame = lv_obj_create(st.detail.detail_cover_host);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, st.detail.detail_cover_fw, st.detail.detail_cover_fh);
    lv_obj_set_style_pad_all(frame, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    StyleBookCoverFrame(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(frame);
    lv_obj_t* img = lv_image_create(frame);
    lv_image_set_src(img, &st.detail.detail_cover.dsc);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void UpdateDetailMetaLabels(const reader::BookInfo& shown) {
    auto& st = Book_State();
    if (st.detail.detail_title_lbl != nullptr && lv_obj_is_valid(st.detail.detail_title_lbl)) {
        lv_label_set_text(st.detail.detail_title_lbl, shown.title.c_str());
    }
    if (st.detail.detail_meta_lbl != nullptr && lv_obj_is_valid(st.detail.detail_meta_lbl)) {
        // 详情页 meta 槽改为作者行；无作者显示「未知」，保持单行槽高度
        lv_label_set_text(st.detail.detail_meta_lbl, shown.author.empty() ? Lang::Strings::COMMON_UNKNOWN : shown.author.c_str());
    }
}

void AsyncDetailCoverDone(void* user_data) {
    auto* msg = static_cast<DetailCoverDoneMsg*>(user_data);
    auto& st = Book_State();

    if (!msg->cover_ok) {
        RememberCoverEmbedMiss(msg->path);
    }

    if (msg->token != st.detail.detail_cover_token.load()) {
        delete msg->cover;
        msg->cover = nullptr;
        delete msg;
        return;
    }

    const bool ui_gone = st.detail.detail_scr == nullptr || !lv_obj_is_valid(st.detail.detail_scr) ||
                         st.detail.detail_cover_host == nullptr || !lv_obj_is_valid(st.detail.detail_cover_host);
    if (ui_gone) {
        delete msg->cover;
        msg->cover = nullptr;
        delete msg;
        return;
    }

    if (st.shelf.selected >= 0 && st.shelf.selected < static_cast<int>(st.shelf.books.size()) &&
        st.shelf.books[static_cast<size_t>(st.shelf.selected)].path == msg->path) {
        if (!msg->title.empty()) {
            st.shelf.books[static_cast<size_t>(st.shelf.selected)].title = msg->title;
        }
        st.shelf.books[static_cast<size_t>(st.shelf.selected)].author = msg->author;
        UpdateDetailMetaLabels(st.shelf.books[static_cast<size_t>(st.shelf.selected)]);
    }

    if (msg->cover != nullptr && msg->cover_ok && !msg->cover->empty()) {
        st.detail.detail_cover = std::move(*msg->cover);
        delete msg->cover;
        msg->cover = nullptr;
        ApplyDetailCoverImage();
    } else {
        delete msg->cover;
        msg->cover = nullptr;
    }
    delete msg;
}

void DetailCoverWorker(void* arg) {
    auto* ctx = static_cast<DetailCoverWorkerArg*>(arg);

    auto* cover = new reader::RasterImage();
    reader::BookInfo info = ctx->info;
    // 始终抽一次；token 只决定是否刷 UI（取消后仍记 miss，避免回书架再抽）
    const bool ok = BookCoverLoader_LoadBookDetailCover(info, ctx->max_w, ctx->max_h, *cover) &&
                    !cover->empty();

    auto* done = new DetailCoverDoneMsg{};
    done->token = ctx->token;
    done->path = ctx->info.path;
    done->title = info.title;
    done->author = info.author;
    done->cover_ok = ok;
    if (ok) {
        done->cover = cover;
    } else {
        delete cover;
        done->cover = nullptr;
    }

    if (!ScreenLvAsync(AsyncDetailCoverDone, done)) {
        if (!ok) {
            PostCoverEmbedMiss(ctx->info.path);
        }
        delete done->cover;
        delete done;
    }
    delete ctx;
    vTaskDelete(nullptr);
}

// 无旁路时后台读文件内封面；有旁路则 CreateDetailScreen 已同步显示。
void StartDetailCoverWorker(int book_index) {
    auto& st = Book_State();
    if (book_index < 0 || book_index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    const uint32_t token = st.detail.detail_cover_token.fetch_add(1) + 1;
    st.detail.detail_format = st.shelf.books[static_cast<size_t>(book_index)].format;

    auto* ctx = new DetailCoverWorkerArg{};
    ctx->token = token;
    ctx->info = st.shelf.books[static_cast<size_t>(book_index)];
    ctx->max_w = CoverFrameInner(st.detail.detail_cover_fw);
    ctx->max_h = CoverFrameInner(st.detail.detail_cover_fh);
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        DetailCoverWorker, "book_cover", kDetailCoverWorkerStack, ctx, tskIDLE_PRIORITY + 2, nullptr,
        0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "book_cover task failed; keep placeholder");
        delete ctx;
        st.detail.detail_cover_token.fetch_add(1);
    }
}

