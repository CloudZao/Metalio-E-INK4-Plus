#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <sys/stat.h>
#include "infra/tasks/task_create.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "business/reader/book_progress_cache.h"
#include "business/reader/list_thumbnail_cache.h"
#include "business/reader/reader.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "business/reader/book_cover_sidecar.h"
#include "ui/apps/book/cover/book_cover_style.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

struct DetailCoverWork : ScreenUiCompletion {
    uint32_t token = 0;
    BookInfo info;
    RasterImage cover;
    int max_w = 0, max_h = 0;
    std::atomic<bool> abort{false};  // 在途工作独立持有，不由后续请求重置
    TaskHandle_t waiter = nullptr;  // 结果回收后才允许处理下一张封面
};

struct ListCoverDoneMsg : ScreenUiCompletion {
    uint32_t token = 0;             // 请求代次，用于丢弃离页或翻页后的结果
    size_t target = 0;              // UI 目标表索引，后台不持有控件或像素槽指针。
    TaskHandle_t waiter = nullptr;  // UI 消费后通知仍存活的唯一 worker。
    RasterImage cover;              // UI 接管的封面像素
    BookInfo info;                  // 后台获取的文件属性，随同代次校验后应用
    bool metadata_ready = false;    // stat 成功后才覆盖现有属性
    bool progress_ready = false;    // 已查询进度，允许更新徽标
};

struct ListCoverItem {
    BookInfo info;      // 当前可见书籍的属性快照
    int max_w = 0;      // 封面目标最大宽度，零表示仅补属性
    int max_h = 0;      // 封面目标最大高度，零表示仅补属性
    size_t target = 0;  // 同一代次中的 UI 目标下标
};

struct ListCoverFillWork {
    uint32_t token = 0;                // 请求代次，用于丢弃离页或翻页后的结果
    bool load_metadata = false;        // 书架名称页在首帧后补全文件属性
    bool load_progress = false;        // 在 UI 线程快照偏好
    std::vector<ListCoverItem> items;  // 当前页可见项，按显示顺序补全
};

struct ListCoverTarget {
    lv_obj_t* host = nullptr;                  // 仅 UI 访问，DELETE 时清空。
    RasterImage* slot = nullptr;               // 非拥有，须与 host 生命周期一致。
    BookFormat format = BookFormat::kUnknown;  // 对应占位封面的格式标签
};

static std::mutex s_list_work_mutex;
static ListCoverFillWork* s_list_posted = nullptr;
static TaskHandle_t s_list_task = nullptr;
static uint32_t s_list_targets_token = 0;
static std::vector<ListCoverTarget> s_list_targets;
static std::mutex s_detail_work_mutex;
static std::unique_ptr<DetailCoverWork> s_detail_posted;
static DetailCoverWork* s_detail_active = nullptr;
static TaskHandle_t s_detail_task = nullptr;

static void ListCoverHostDeleted(lv_event_t* event) {
    auto* host = static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (auto& target : s_list_targets) {
        if (target.host == host) {
            target.host = nullptr;
            target.slot = nullptr;
        }
    }
}

static void ApplyListCoverImage(lv_obj_t* host, RasterImage& cover, BookFormat format) {
    if (host == nullptr || !lv_obj_is_valid(host) || cover.empty()) {
        return;
    }
    cover.BindDsc();
    lv_obj_t* img = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(host); ++i) {
        auto* child = lv_obj_get_child(host, i);
        if (lv_obj_check_type(child, &lv_image_class)) {
            img = child;
        } else if (lv_obj_check_type(child, &lv_label_class) &&
                   std::strcmp(lv_label_get_text(child), FormatLabel(format)) == 0) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (img == nullptr) {
        img = lv_image_create(host);
    }
    lv_image_set_src(img, &cover.dsc);
    EpdSetContentRole(img, EpdContentRole::Cover);
    lv_obj_move_to_index(img, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void AsyncListCoverDone(void* user_data) {
    std::unique_ptr<ListCoverDoneMsg> msg(static_cast<ListCoverDoneMsg*>(user_data));
    struct NotifyWorker {
        TaskHandle_t waiter;
        ~NotifyWorker() { xTaskNotifyGive(waiter); }
    } notify{msg->waiter};
    auto& st = Book_State();
    if (msg->token == st.shelf.list_cover_token.load() && msg->token == s_list_targets_token &&
        msg->target < s_list_targets.size()) {
        auto& target = s_list_targets[msg->target];
        if (target.host != nullptr && lv_obj_is_valid(target.host)) {
            if (msg->metadata_ready) {
                ApplyShelfEntryMetadata(msg->info, target.host, msg->progress_ready);
            }
            if (target.slot && !msg->cover.empty()) {
                *target.slot = std::move(msg->cover);
                ApplyListCoverImage(target.host, *target.slot, target.format);
            }
        }
    }

}

static void RunListCoverWork(ListCoverFillWork* work) {
    auto& st = Book_State();
    for (auto& item : work->items) {
        if (work->token != st.shelf.list_cover_token.load()) {
            break;
        }
        auto done = std::unique_ptr<ListCoverDoneMsg>(new (std::nothrow) ListCoverDoneMsg{});
        if (!done) {
            ESP_LOGW(TAG, "visible entry result allocation failed; keep placeholders");
            break;
        }
        try {
            SdAccessTransaction access;
            if (!access || SdAccessIsQuiescing()) {
                break;
            }
            done->metadata_ready = !work->load_metadata;
            struct stat status{};
            if (work->load_metadata && stat(item.info.path.c_str(), &status) == 0) {
                item.info.file_size = status.st_size;
                item.info.mtime = status.st_mtime;
                done->metadata_ready = true;
            } else if (work->load_metadata) {
                ESP_LOGW(TAG, "visible entry stat failed errno=%d; keep placeholder", errno);
            }
            if (work->token != st.shelf.list_cover_token.load()) {
                break;
            }
            if (done->metadata_ready && work->load_progress) {
                BookSession::ProgressPeek progress;
                if (!BookProgressCacheTryGet(item.info.path.c_str(), progress)) {
                    progress = BookSession::PeekProgress(item.info.path.c_str());
                    BookProgressCacheUpsert(item.info.path.c_str(), progress);
                }
                done->progress_ready = true;
            }
            if (work->token != st.shelf.list_cover_token.load()) {
                break;
            }
            if (done->metadata_ready && item.max_w > 0 && item.max_h > 0) {
                const bool cached = GetListThumbnail(item.info.path.c_str(), item.max_w, item.max_h,
                                                     item.info.file_size, item.info.mtime, done->cover);
                if (!cached && BookCoverLoader_LoadBookDetailCover(item.info, item.max_w, item.max_h, done->cover) &&
                    !done->cover.empty() && work->token == st.shelf.list_cover_token.load()) {
                    CacheListThumbnail(item.info.path.c_str(), item.max_w, item.max_h, item.info.file_size,
                                       item.info.mtime, done->cover);
                }
            }
            done->info = std::move(item.info);
        } catch (const std::bad_alloc&) {
            ESP_LOGW(TAG, "visible entry allocation failed; keep placeholder");
            continue;
        }
        if (work->token != st.shelf.list_cover_token.load()) {
            break;
        }
        if (!done->metadata_ready && done->cover.empty()) {
            continue;
        }
        done->token = work->token;
        done->target = item.target;
        done->waiter = xTaskGetCurrentTaskHandle();
        done->apply = [](ScreenUiCompletion* completion) { AsyncListCoverDone(completion); };
        ScreenCompleteWork(done.release());
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void ListCoverFillWorker(void*) {
    for (;;) {
        std::unique_ptr<ListCoverFillWork> work;
        {
            std::lock_guard<std::mutex> lock(s_list_work_mutex);
            work.reset(s_list_posted);
            s_list_posted = nullptr;
            if (!work) {
                s_list_task = nullptr;
            }
        }
        if (!work) {
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        RunListCoverWork(work.get());
    }
}

void FillDetailCoverPlaceholder(lv_obj_t* host, BookFormat format) {
    auto& st = Book_State();
    if (host == nullptr) {
        return;
    }
    lv_obj_clean(host);
    lv_obj_t* ph = lv_obj_create(host);
    lv_obj_remove_style_all(ph);
    lv_obj_set_size(ph, st.detail.detail_cover_fw, st.detail.detail_cover_fh);
    lv_obj_set_style_pad_all(ph, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(ph, UiColorSurface(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    StyleBookCoverFrame(ph);
    lv_obj_t* ph_lbl = lv_label_create(ph);
    lv_label_set_text(ph_lbl, FormatLabel(format));
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
    lv_obj_set_style_bg_color(frame, UiColorSurface(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    StyleBookCoverFrame(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(frame);
    lv_obj_t* img = lv_image_create(frame);
    lv_image_set_src(img, &st.detail.detail_cover.dsc);
    EpdSetContentRole(img, EpdContentRole::Cover);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void UpdateDetailMetaLabels(const BookInfo& shown) {
    auto& st = Book_State();
    if (st.detail.detail_title_lbl != nullptr && lv_obj_is_valid(st.detail.detail_title_lbl)) {
        lv_label_set_text(st.detail.detail_title_lbl, shown.title.c_str());
    }
    if (st.detail.detail_meta_lbl != nullptr && lv_obj_is_valid(st.detail.detail_meta_lbl)) {
        // 详情页 meta 槽改为作者行；无作者显示「未知」，保持单行槽高度
        lv_label_set_text(st.detail.detail_meta_lbl,
                          shown.author.empty() ? LangString_COMMON_UNKNOWN : shown.author.c_str());
    }
}

static void ApplyDetailCover(ScreenUiCompletion* completion) {
    std::unique_ptr<DetailCoverWork> result(static_cast<DetailCoverWork*>(completion));
    struct NotifyWorker {
        TaskHandle_t waiter;
        ~NotifyWorker() { xTaskNotifyGive(waiter); }
    } notify{result->waiter};
    auto& state = Book_State();
    if (result->token != state.detail.detail_cover_token.load() || !state.detail.detail_scr ||
        !state.detail.detail_cover_host || !lv_obj_is_valid(state.detail.detail_cover_host)) {
        return;
    }
    if (state.shelf.selected >= 0 && state.shelf.selected < int(state.shelf.books.size()) &&
        state.shelf.books[state.shelf.selected].path == result->info.path) {
        auto& info = state.shelf.books[state.shelf.selected];
        if (!result->info.title.empty()) {
            info.title = std::move(result->info.title);
        }
        info.author = std::move(result->info.author);
        UpdateDetailMetaLabels(info);
    }
    if (!result->cover.empty()) {
        state.detail.detail_cover = std::move(result->cover);
        ApplyDetailCoverImage();
    }
}

static void DetailCoverWorker(void*) {
    for (;;) {
        std::unique_ptr<DetailCoverWork> work;
        {
            std::lock_guard<std::mutex> lock(s_detail_work_mutex);
            work = std::move(s_detail_posted);
            s_detail_active = work.get();
            if (!work) {
                s_detail_task = nullptr;
            }
        }
        if (!work) {
            break;
        }
        try {
            BookCoverLoader_LoadBookDetailCover(work->info, work->max_w, work->max_h, work->cover, &work->abort);
        } catch (const std::bad_alloc&) {
            work->cover.Reset();
            ESP_LOGW(TAG, "operation=detail_cover stage=execute result=no_memory");
        }
        {
            std::lock_guard<std::mutex> lock(s_detail_work_mutex);
            s_detail_active = nullptr;
        }
        if (work->abort.load()) {
            continue;
        }
        work->waiter = xTaskGetCurrentTaskHandle();
        ScreenCompleteWork(work.release());
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    vTaskDeleteWithCaps(nullptr);
}

void CancelDetailCoverWorker() {
    Book_State().detail.detail_cover_token.fetch_add(1);
    std::unique_ptr<DetailCoverWork> discarded;
    {
        std::lock_guard<std::mutex> lock(s_detail_work_mutex);
        if (s_detail_active) {
            s_detail_active->abort.store(true);
        }
        discarded = std::move(s_detail_posted);
    }
}

void StartDetailCoverWorker(int book_index) try {
    auto& state = Book_State();
    if (book_index < 0 || book_index >= int(state.shelf.books.size())) {
        return;
    }
    CancelDetailCoverWorker();
    auto work = std::make_unique<DetailCoverWork>();
    work->token = state.detail.detail_cover_token.load();
    work->apply = ApplyDetailCover;
    work->info = state.shelf.books[book_index];
    state.detail.detail_format = work->info.format;
    work->max_w = CoverFrameInner(state.detail.detail_cover_fw);
    work->max_h = CoverFrameInner(state.detail.detail_cover_fh);
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(s_detail_work_mutex);
        s_detail_posted = std::move(work);
        if (s_detail_task == nullptr &&
            FirmwareCreateTask(DetailCoverWorker, "book_cover", kDetailCoverWorkerStack, nullptr,
                               tskIDLE_PRIORITY + 2, &s_detail_task, 0,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_detail_task = nullptr;
            work = std::move(s_detail_posted);
            failed = true;
        }
    }
    if (failed) {
        ESP_LOGW(TAG, "operation=detail_cover stage=create_task result=unavailable");
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "operation=detail_cover stage=submit result=no_memory");
}

void EnqueueListCoverFill(const BookInfo& info, int max_w, int max_h, lv_obj_t* host, RasterImage* slot) try {
    if (host == nullptr || info.path.empty() || (slot && (max_w <= 0 || max_h <= 0))) {
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
    ESP_LOGW(TAG, "cover request allocation failed; keep placeholder");
}

void ScheduleListCoverFill() {
    auto& st = Book_State();
    if (st.shelf.list_cover_pending.empty()) {
        return;
    }
    const uint32_t token = st.shelf.list_cover_token.load();
    if (s_list_targets_token != token) {
        s_list_targets.clear();
        s_list_targets_token = token;
    }
    const size_t previous_targets = s_list_targets.size();
    auto work = std::unique_ptr<ListCoverFillWork>(new (std::nothrow) ListCoverFillWork{});
    if (!work) {
        ESP_LOGW(TAG, "cover batch allocation failed; keep placeholders");
        return;
    }
    work->token = token;
    work->load_metadata = st.shelf.back_root == BookUiState::NavRoot::kShelf && st.shelf.list_scr &&
                          lv_screen_active() == st.shelf.list_scr;
    work->load_progress = work->load_metadata && BookReaderPrefsShowShelfProgress() != 0;
    std::unique_ptr<ListCoverFillWork> replaced;
    bool start_failed = false;
    try {
        work->items.reserve(st.shelf.list_cover_pending.size());
        for (const auto& pending : st.shelf.list_cover_pending) {
            size_t target = 0;
            while (target < s_list_targets.size() &&
                   (s_list_targets[target].host != pending.host || s_list_targets[target].slot != pending.slot)) {
                ++target;
            }
            if (target == s_list_targets.size()) {
                s_list_targets.push_back({pending.host, pending.slot, pending.info.format});
                lv_obj_add_event_cb(pending.host, ListCoverHostDeleted, LV_EVENT_DELETE, nullptr);
            }
            work->items.push_back({pending.info, pending.max_w, pending.max_h, target});
        }
        {
            std::lock_guard<std::mutex> lock(s_list_work_mutex);
            if (s_list_posted != nullptr && s_list_posted->token == token) {
                work->items.reserve(work->items.size() + s_list_posted->items.size());
                for (auto& item : s_list_posted->items) {
                    const bool duplicate =
                        std::any_of(work->items.begin(), work->items.end(),
                                    [&](const ListCoverItem& current) { return current.target == item.target; });
                    if (!duplicate) {
                        work->items.push_back(std::move(item));
                    }
                }
            }
            replaced.reset(s_list_posted);
            s_list_posted = work.release();
            st.shelf.list_cover_pending.clear();
            if (s_list_task == nullptr &&
                (FirmwareCreateTask(ListCoverFillWorker, "list_cover", kListCoverWorkerStack, nullptr,
                                                tskIDLE_PRIORITY + 1, &s_list_task, 0,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)) {
                s_list_task = nullptr;
                work.reset(s_list_posted);
                s_list_posted = nullptr;
                start_failed = true;
            }
        }
        if (start_failed) {
            ESP_LOGW(TAG, "list_cover task failed; keep placeholders");
        }
    } catch (const std::bad_alloc&) {
        s_list_targets.resize(previous_targets);
        ESP_LOGW(TAG, "cover batch allocation failed; keep placeholders");
    }
}
