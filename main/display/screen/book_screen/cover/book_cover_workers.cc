#include "epd_content_policy.h"
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
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <mutex>
#include <new>
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
    size_t target = 0; // UI 目标表索引，后台不持有控件或像素槽指针。
    TaskHandle_t waiter = nullptr; // UI 消费后通知仍存活的唯一 worker。
    reader::RasterImage cover;
};

struct ListCoverItem {
    reader::BookInfo info;
    int max_w = 0,max_h = 0;
    size_t target = 0;
};

struct ListCoverFillWork {
    uint32_t token = 0;
    std::vector<ListCoverItem> items;
};

struct ListCoverTarget {
    lv_obj_t* host = nullptr; // 仅 UI 访问，DELETE 时清空。
    reader::RasterImage* slot = nullptr; // 非拥有，须与 host 生命周期一致。
    reader::BookFormat format = reader::BookFormat::kUnknown;
};

static std::mutex s_list_work_mutex;
static ListCoverFillWork* s_list_posted = nullptr;
static TaskHandle_t s_list_task = nullptr;
static uint32_t s_list_targets_token = 0;
static std::vector<ListCoverTarget> s_list_targets;

void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot) try {
    if (host == nullptr || slot == nullptr || max_w <= 0 || max_h <= 0 || info.path.empty()) {
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
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG,"cover request allocation failed; keep placeholder");
}

static void ListCoverHostDeleted(lv_event_t* event) {
    auto* host=static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (auto& target:s_list_targets) {
        if (target.host==host) {
            target.host=nullptr;
            target.slot=nullptr;
        }
    }
}

static void ApplyListCoverImage(lv_obj_t* host, reader::RasterImage& cover,reader::BookFormat format) {
    if (host == nullptr || !lv_obj_is_valid(host) || cover.empty()) {
        return;
    }
    cover.BindDsc();
    lv_obj_t* img=nullptr;
    for (uint32_t i=0;i<lv_obj_get_child_count(host);++i) {
        auto* child=lv_obj_get_child(host,i);
        if (lv_obj_check_type(child,&lv_image_class)) img=child;
        else if (lv_obj_check_type(child,&lv_label_class) &&
                 std::strcmp(lv_label_get_text(child),reader::FormatLabel(format))==0) {
            lv_obj_add_flag(child,LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (img==nullptr) img=lv_image_create(host);
    lv_image_set_src(img, &cover.dsc);
    EpdSetContentRole(img,EpdContentRole::Cover);
    lv_obj_move_to_index(img,0);
    lv_obj_clear_flag(img,LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void AsyncListCoverDone(void* user_data) {
    std::unique_ptr<ListCoverDoneMsg> msg(static_cast<ListCoverDoneMsg*>(user_data));
    auto& st = Book_State();
    if (msg->token==st.shelf.list_cover_token.load() && msg->token==s_list_targets_token &&
        msg->target<s_list_targets.size()) {
        auto& target=s_list_targets[msg->target];
        if (target.host!=nullptr && lv_obj_is_valid(target.host) && target.slot!=nullptr) {
            *target.slot=std::move(msg->cover);
            ApplyListCoverImage(target.host,*target.slot,target.format);
        }
    }
    // worker 不接收其他通知，且在本次确认前不会退出或投递第二张图片。
    xTaskNotifyGive(msg->waiter);
}

static void RunListCoverWork(ListCoverFillWork* work) {
    auto& st = Book_State();
    for (auto& item:work->items) {
        if (work->token!=st.shelf.list_cover_token.load()) break;
        auto done=std::unique_ptr<ListCoverDoneMsg>(new(std::nothrow) ListCoverDoneMsg{});
        if (!done) break;
        bool ok=false;
        try {
            ok=BookCoverLoader_LoadBookDetailCover(item.info,item.max_w,item.max_h,done->cover) &&
                !done->cover.empty();
        } catch (const std::bad_alloc&) {
            ESP_LOGW(TAG,"cover decode allocation failed; keep placeholder");
        }
        if (work->token!=st.shelf.list_cover_token.load()) break;
        if (!ok) continue;
        done->token = work->token;
        done->target = item.target;
        done->waiter = xTaskGetCurrentTaskHandle();
        while (work->token==st.shelf.list_cover_token.load()) {
            if (ScreenLvAsync(AsyncListCoverDone,done.get())) {
                done.release();
                ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void ListCoverFillWorker(void*) {
    for (;;) {
        std::unique_ptr<ListCoverFillWork> work;
        {
            std::lock_guard<std::mutex> lock(s_list_work_mutex);
            work.reset(s_list_posted);
            s_list_posted=nullptr;
            if (!work) s_list_task=nullptr;
        }
        if (!work) {
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        RunListCoverWork(work.get());
    }
}

void ScheduleListCoverFill() {
    auto& st = Book_State();
    if (st.shelf.list_cover_pending.empty()) return;
    const uint32_t token=st.shelf.list_cover_token.load();
    if (s_list_targets_token!=token) {
        s_list_targets.clear();
        s_list_targets_token=token;
    }
    const size_t previous_targets=s_list_targets.size();
    auto work=std::unique_ptr<ListCoverFillWork>(new(std::nothrow) ListCoverFillWork{});
    if (!work) return;
    work->token=token;
    std::unique_ptr<ListCoverFillWork> replaced;
    bool start_failed=false;
    try {
        work->items.reserve(st.shelf.list_cover_pending.size());
        for (const auto& pending:st.shelf.list_cover_pending) {
            size_t target=0;
            while (target<s_list_targets.size() &&
                   (s_list_targets[target].host!=pending.host || s_list_targets[target].slot!=pending.slot)) ++target;
            if (target==s_list_targets.size()) {
                s_list_targets.push_back({pending.host,pending.slot,pending.info.format});
                lv_obj_add_event_cb(pending.host,ListCoverHostDeleted,LV_EVENT_DELETE,nullptr);
            }
            work->items.push_back({pending.info,pending.max_w,pending.max_h,target});
        }
        {
            std::lock_guard<std::mutex> lock(s_list_work_mutex);
            if (s_list_posted!=nullptr && s_list_posted->token==token) {
                work->items.reserve(work->items.size()+s_list_posted->items.size());
                for (auto& item:s_list_posted->items) {
                    const bool duplicate=std::any_of(work->items.begin(),work->items.end(),
                        [&](const ListCoverItem& current) { return current.target==item.target; });
                    if (!duplicate) work->items.push_back(std::move(item));
                }
            }
            replaced.reset(s_list_posted);
            s_list_posted=work.release();
            st.shelf.list_cover_pending.clear();
            if (s_list_task==nullptr && xTaskCreatePinnedToCoreWithCaps(
                ListCoverFillWorker,"list_cover",kListCoverWorkerStack,nullptr,tskIDLE_PRIORITY+1,
                &s_list_task,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
                s_list_task=nullptr;
                work.reset(s_list_posted);
                s_list_posted=nullptr;
                start_failed=true;
            }
        }
        if (start_failed) ESP_LOGW(TAG,"list_cover task failed; keep placeholders");
    } catch (const std::bad_alloc&) {
        s_list_targets.resize(previous_targets);
        ESP_LOGW(TAG,"cover batch allocation failed; keep placeholders");
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
    EpdSetContentRole(img,EpdContentRole::Cover);
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
    // 失败可能来自 IO 或内存不足，不作为永久缺封面的依据。
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
        delete done->cover;
        delete done;
    }
    delete ctx;
    vTaskDeleteWithCaps(nullptr);
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

