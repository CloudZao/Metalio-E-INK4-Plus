#pragma GCC optimize("O1")

#include "book_screen/reader/book_open_worker.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "lv_adapter_display.h"
#include "reader/book_cover_sidecar.h"
#include "reader/reader.h"
#include "screen_common.h"
#include "vk_key_handler.h"

struct OpenProgressMsg {
    uint32_t token = 0;
    int percent = 0;
};

struct OpenDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct OpenWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;  // 拷贝，避免 Create 重建书库时 vector 失效
};

// worker → Tick：勿 ScreenLvAsyncUrgent 抢锁（Tick 整屏 I1 时可占锁十余秒）
static std::atomic<OpenDoneMsg*> s_pending_open_done{nullptr};

static void AsyncOpenDone(void* user_data);

void BookDrainPendingOpenDone() {
    OpenDoneMsg* msg = s_pending_open_done.exchange(nullptr, std::memory_order_acq_rel);
    if (msg != nullptr) {
        AsyncOpenDone(msg);
    }
}

void ShowOpenProgress(int percent) {
    auto& st = Book_State();
    if (st.reader.content == nullptr) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    const bool need_build = st.reader.open_bar == nullptr || !lv_obj_is_valid(st.reader.open_bar) ||
                            st.reader.open_pct_lbl == nullptr || !lv_obj_is_valid(st.reader.open_pct_lbl);

    if (need_build) {
        st.reader.open_bar = nullptr;
        st.reader.open_pct_lbl = nullptr;
        lv_obj_clean(st.reader.content);
        // 打开进度 UI 用书库边距；避免阅读页边距档位导致 Book_ContentWidth 溢出
        ApplyOverlayListContentPads(0);
        lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_NONE, 0);
        Book_DisableScroll(st.reader.content);

        lv_obj_t* panel = lv_obj_create(st.reader.content);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, Book_ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 18, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(panel);
        lv_obj_center(panel);

        lv_obj_t* title = lv_label_create(panel);
        lv_label_set_text(title, Lang::Strings::BOOK_OPENING);
        lv_obj_set_style_text_font(title, Book_ListFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        // 轨道：白底黑框；指示器：实心黑块（墨水屏清晰）
        lv_coord_t bar_w = kOpenBarW;
        if (bar_w > Book_ContentWidth() - 8) {
            bar_w = Book_ContentWidth() - 8;
        }
        lv_obj_t* bar = lv_bar_create(panel);
        lv_obj_set_size(bar, bar_w, kOpenBarH);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
        lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_bar = bar;

        lv_obj_t* pct = lv_label_create(panel);
        lv_obj_set_style_text_font(pct, Book_ListFont(), 0);
        lv_obj_set_style_text_color(pct, lv_color_black(), 0);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_pct_lbl = pct;
    }

    lv_bar_set_value(st.reader.open_bar, percent, LV_ANIM_OFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(st.reader.open_pct_lbl, buf);
}

static void AsyncOpenProgress(void* user_data) {
    auto* msg = static_cast<OpenProgressMsg*>(user_data);
    auto& st = Book_State();
    if (msg->token == st.reader.open_token.load() && st.reader.opening.load() &&
        st.reader.content != nullptr) {
        ShowOpenProgress(msg->percent);
    }
    delete msg;
}

static void OnOpenProgress(int percent, void* user) {
    auto* msg = new OpenProgressMsg{};
    msg->token = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    msg->percent = percent;
    if (!ScreenLvAsync(AsyncOpenProgress, msg)) {
        delete msg;
    }
}

static void AsyncOpenDone(void* user_data) {
    auto* msg = static_cast<OpenDoneMsg*>(user_data);
    auto& st = Book_State();
    st.reader.opening.store(false);
    ESP_LOGI(TAG, "AsyncOpenDone begin ok=%d", msg->ok ? 1 : 0);

    if (msg->token != st.reader.open_token.load() || st.reader.deferred_cleanup || st.reader.content == nullptr) {
        FinishDeferredCleanup();
        delete msg;
        return;
    }

    if (!msg->ok || !st.reader.session || !st.reader.session->IsOpen()) {
        EndReadingTimeTracking();
        ReleaseBookFont();
        if (st.reader.opening_format == reader::BookFormat::kTxt) {
            Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED_TXT);
        } else if (st.reader.opening_format == reader::BookFormat::kEbook) {
            Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED_CHAPTER);
        } else {
            Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED_EPUB);
        }
        delete msg;
        return;
    }

    // 文档元数据回写书库，下次详情秒开可见 OPF 书名/作者
    if (st.shelf.selected >= 0 && st.shelf.selected < static_cast<int>(st.shelf.books.size()) &&
        st.shelf.books[static_cast<size_t>(st.shelf.selected)].path == st.reader.session->Info().path) {
        auto& bi = st.shelf.books[static_cast<size_t>(st.shelf.selected)];
        if (!st.reader.session->Title().empty()) {
            bi.title = st.reader.session->Title();
        }
        if (!st.reader.session->Author().empty()) {
            bi.author = st.reader.session->Author();
        }
    }

    if (st.reader.settings_resume_after_open) {
        st.reader.settings_resume_after_open = false;
        st.reader.read_chrome = BookUiState::ReadChrome::kReading;
        const int64_t t0 = esp_timer_get_time();
        RenderReaderPage();
        ESP_LOGI(TAG, "AsyncOpenDone RenderReaderPage cost=%lldms",
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000));
        ShowReadSettingsSheet();
        BeginReadingTimeTracking();
        if (st.reader.session->NeedsChapterPageRebuild()) {
            StartChapterPageWorker();
        }
        delete msg;
        return;
    }

    {
        const int64_t t0 = esp_timer_get_time();
        RenderReaderPage();
        ESP_LOGI(TAG, "AsyncOpenDone RenderReaderPage cost=%lldms",
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000));
    }
    BeginReadingTimeTracking();
    if (st.reader.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
    delete msg;
}

void OpenBookWorker(void* arg) {
    auto* ctx = static_cast<OpenWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;

    const uint32_t token_now = st.reader.open_token.load();
    ESP_LOGI(TAG, "book_open worker start token=%u fmt=%d path=%s",
             static_cast<unsigned>(ctx->token), static_cast<int>(ctx->info.format),
             ctx->info.path.c_str());

    if (st.reader.session == nullptr) {
        ESP_LOGE(TAG, "book_open skip: session null");
    } else if (ctx->token != token_now) {
        ESP_LOGW(TAG, "book_open skip: token stale %u!=%u", static_cast<unsigned>(ctx->token),
                 static_cast<unsigned>(token_now));
    } else {
        st.reader.session->SetOpenProgress(OnOpenProgress,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->token)));
        ok = st.reader.session->Open(ctx->info);
        st.reader.session->SetOpenProgress(nullptr, nullptr);
        // 打开路径统一补旁路封面，供书架/详情/目录取用（失败不阻断阅读）
        if (ok && ctx->token == st.reader.open_token.load() &&
            (ctx->info.format == reader::BookFormat::kEbook ||
             ctx->info.format == reader::BookFormat::kEpub)) {
            char sidecar[192];
            if (reader::BookCoverSidecarPath(ctx->info.path.c_str(), sidecar, sizeof(sidecar)) &&
                access(sidecar, R_OK) != 0) {
                reader::SaveBookCoverSidecar(ctx->info.path.c_str());
            }
        }
    }

    ESP_LOGI(TAG, "book_open worker done ok=%d", ok ? 1 : 0);

    auto* done = new OpenDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    // 无锁投递：Tick 持锁后先 BookDrainPendingOpenDone，再跑 timer_handler
    OpenDoneMsg* stale = s_pending_open_done.exchange(done, std::memory_order_acq_rel);
    if (stale != nullptr) {
        delete stale;
    }
    delete ctx;
    vTaskDelete(nullptr);
}

void StartOpenWorker(int book_index) {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        const char* path = (book_index >= 0 && book_index < static_cast<int>(st.shelf.books.size()))
                               ? st.shelf.books[static_cast<size_t>(book_index)].path.c_str()
                               : "-";
        ESP_LOGW(TAG, "StartOpenWorker ignored: already opening token=%u req_idx=%d path=%s",
                 static_cast<unsigned>(st.reader.open_token.load()), book_index, path);
        return;
    }
    if (book_index < 0 || book_index >= static_cast<int>(st.shelf.books.size()) || !st.reader.session) {
        ESP_LOGE(TAG, "StartOpenWorker bad state idx=%d books=%u session=%d", book_index,
                 static_cast<unsigned>(st.shelf.books.size()), st.reader.session ? 1 : 0);
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED);
        return;
    }

    const uint32_t token = st.reader.open_token.fetch_add(1) + 1;
    st.reader.opening.store(true);
    st.reader.deferred_cleanup = false;
    st.reader.opening_format = st.shelf.books[static_cast<size_t>(book_index)].format;
    ShowOpenProgress(0);

    auto* ctx = new OpenWorkerArg{};
    ctx->token = token;
    ctx->info = st.shelf.books[static_cast<size_t>(book_index)];
    ESP_LOGI(TAG, "StartOpenWorker token=%u fmt=%d path=%s", static_cast<unsigned>(token),
             static_cast<int>(ctx->info.format), ctx->info.path.c_str());
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        OpenBookWorker, "book_open", kOpenWorkerStack, ctx, tskIDLE_PRIORITY + 2, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "book_open task failed: int free=%u spiram=%u stack=%u",
                 static_cast<unsigned>(free_int), static_cast<unsigned>(free_spiram),
                 static_cast<unsigned>(kOpenWorkerStack));
        delete ctx;
        st.reader.opening.store(false);
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED_TASK);
    }
}

void StartReadAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = Book_State();
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    // 编排/打开 worker 仍握旧 session：勿 CreateReaderScreen 拆掉
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("start read");
        return;
    }
    st.shelf.selected = index;
    ESP_LOGI(TAG, "start read idx=%d path=%s", index,
             st.shelf.books[static_cast<size_t>(index)].path.c_str());
    CancelDetailCoverLoad();
    // 勿在此 Reset detail_cover：旧详情屏异步删除前仍引用封面缓冲
    ScreenLoadReplace(CreateReaderScreen(st.shelf.books[static_cast<size_t>(index)]));
}

void BackToLibraryAsync(void* /*user_data*/) {
    // 详情 vk_home / vk_prev → 书架或阅读首页。
    CancelDetailCoverLoad();
    // Create / CreateBookshelf 统一收尾正文 session / SD 字库（含 opening 中途离开的 deferred）。
    // detail_scr / cover 由详情屏 LV_EVENT_DELETE 清理，勿在此 Reset。
    if (Book_State().shelf.back_root == BookUiState::NavRoot::kShelf) {
        ScreenLoadReplace(CreateBookshelfScreen());
    } else {
        ScreenLoadReplace(BookScreen::Create());
    }
}

void BackToDetailAsync(void* /*user_data*/) {
    auto& st = Book_State();
    const int index = st.shelf.selected;
    // 与进百问同源：open_token 失效 worker 回调；opening 时 deferred_cleanup，避免 UAF/泄漏
    ReleaseReaderSessionForLeave();
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        ScreenLoadReplace(BookScreen::Create());
        return;
    }
    ScreenLoadReplace(CreateDetailScreen(index));
}

void OpenDetailAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = Book_State();
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    st.shelf.selected = index;
    const char* screen = VkKey_ActiveScreen();
    if (screen != nullptr && std::strcmp(screen, kScreenBookshelf) == 0) {
        st.shelf.back_root = BookUiState::NavRoot::kShelf;
    } else {
        st.shelf.back_root = BookUiState::NavRoot::kHome;
    }
    ScreenLoadReplace(CreateDetailScreen(index));
}

void OpenBookshelfAsync(void* /*user_data*/) {
    Book_State().shelf.list_page = 0;
    ScreenLoadReplace(CreateBookshelfScreen());
}

void BackToReadingHomeAsync(void* /*user_data*/) {
    ScreenLoadReplace(BookScreen::Create());
}

void OnContinueReadingClicked(lv_event_t* e) {
    auto& st = Book_State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("home continue CTA");
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        ESP_LOGW(TAG, "home continue CTA bad index=%d books=%u", index,
                 static_cast<unsigned>(st.shelf.books.size()));
        return;
    }
    ESP_LOGI(TAG, "home continue CTA idx=%d path=%s", index,
             st.shelf.books[static_cast<size_t>(index)].path.c_str());
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    st.shelf.selected = index;
    lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

void OnOpenShelfClicked(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    lv_async_call(OpenBookshelfAsync, nullptr);
}

void OnDetailStartClicked(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("detail continue CTA");
        return;
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        ESP_LOGW(TAG, "detail continue CTA bad sel=%d books=%u", st.shelf.selected,
                 static_cast<unsigned>(st.shelf.books.size()));
        return;
    }
    ESP_LOGI(TAG, "detail continue CTA sel=%d path=%s", st.shelf.selected,
             st.shelf.books[static_cast<size_t>(st.shelf.selected)].path.c_str());
    lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(st.shelf.selected)));
}

void DispatchTapZoneAction(BookTapZoneAction action) {
    auto& st = Book_State();
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
        return;
    }
    switch (action) {
        case BookTapZoneAction::kPrev:
            QueueReaderPageTurn(-1);
            break;
        case BookTapZoneAction::kNext:
            QueueReaderPageTurn(1);
            break;
        case BookTapZoneAction::kMenu:
            ShowReadSettingsSheet();
            break;
        case BookTapZoneAction::kBack:
            // 与 vk_home 同路：勿在 CLICKED 栈里同步拆 session
            BookLvAsync(BackToDetailAsync);
            break;
        case BookTapZoneAction::kToc:
            OpenReadToc();
            break;
        case BookTapZoneAction::kFullRefresh:
            if (auto* disp = LVAdapterDisplay::Instance()) {
                disp->RequestNextFullRefresh();
            }
            if (st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr)) {
                lv_obj_invalidate(st.reader.read_scr);
            } else if (st.reader.content != nullptr && lv_obj_is_valid(st.reader.content)) {
                lv_obj_invalidate(st.reader.content);
            }
            break;
        case BookTapZoneAction::kNone:
        default:
            break;
    }
}

