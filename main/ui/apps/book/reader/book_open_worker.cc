
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "infra/tasks/task_create.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <unistd.h>
#include "assets/lang_screen.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/apps/book/reader/book_open_worker.h"
#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "business/reader/book_cover_sidecar.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_tap_zones.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/apps/book/shelf/book_home.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

struct OpenProgressMsg {
    uint32_t token = 0;
    int percent = 0;
};

struct OpenWorkerArg : ScreenUiCompletion {
    uint32_t token = 0;
    bool ok = false;
    BookInfo info; // 接纳时拷贝，后台结束后释放字符串再交付结果
};

static std::unique_ptr<OpenWorkerArg> s_deferred_open; // 仅在UI任务持有，待机后恢复呈现

static void AsyncOpenDone(void* user_data);

static UiModal s_window_modal;

static void AsyncOpenProgress(void* user_data) {
    auto* msg = static_cast<OpenProgressMsg*>(user_data);
    auto& st = Book_State();
    if (msg->token == st.reader.open_token.load() && st.reader.opening.load() && !st.reader.standby_paused &&
        st.reader.content != nullptr) {
        ShowOpenProgress(msg->percent);
    }
    delete msg;
}

static void OnOpenProgress(int percent, void* user) {
    auto* msg = new (std::nothrow) OpenProgressMsg{};
    if (!msg) {
        return;
    }
    msg->token = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    msg->percent = percent;
    if (!ScreenLvAsync(AsyncOpenProgress, msg)) {
        delete msg;
    }
}

static void AsyncOpenDone(void* user_data) {
    std::unique_ptr<OpenWorkerArg> msg(static_cast<OpenWorkerArg*>(user_data));
    auto& st = Book_State();
    st.reader.opening.store(false);
    ESP_LOGI(TAG, "AsyncOpenDone begin ok=%d", msg->ok ? 1 : 0);

    if (msg->token != st.reader.open_token.load() || st.reader.deferred_cleanup || st.reader.content == nullptr) {
        FinishDeferredCleanup();
        return;
    }

    if (!msg->ok || !st.reader.session || !st.reader.session->IsOpen()) {
        EndReadingTimeTracking();
        ReleaseBookFont();
        const auto error = st.reader.session ? st.reader.session->Error() : BookReadError::kInvalidData;
        const char* message = LangString_BOOK_OPEN_FAILED_DATA;
        switch (error) {
            case BookReadError::kFileIo:
                message = LangString_BOOK_OPEN_FAILED_IO;
                break;
            case BookReadError::kNoMemory:
                message = LangString_BOOK_OPEN_FAILED_MEMORY;
                break;
            case BookReadError::kCacheIo:
                message = LangString_BOOK_OPEN_FAILED_CACHE;
                break;
            case BookReadError::kUnsupported:
                message = LangString_BOOK_OPEN_FAILED_UNSUPPORTED;
                break;
            case BookReadError::kCancelled:
                message = LangString_BOOK_OPEN_CANCELLED;
                break;
            default:
                break;
        }
        Book_ShowMessage(st.reader.content, message);
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

    const bool show_settings = st.reader.settings_resume_after_open;
    st.reader.settings_resume_after_open = false;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    RenderReaderPage();
    if (show_settings) {
        ShowReadSettingsSheet(false);
    }
    BeginReadingTimeTracking();
}

static void ApplyOpenCompletion(ScreenUiCompletion* completion) {
    std::unique_ptr<OpenWorkerArg> result(static_cast<OpenWorkerArg*>(completion));
    auto& state = Book_State();
    state.reader.opening.store(false);
    BookRequestUiWork();
    if (state.reader.standby_paused && !state.reader.deferred_cleanup && state.reader.content &&
        result->token == state.reader.open_token.load()) {
        s_deferred_open = std::move(result);
        return;
    }
    AsyncOpenDone(result.release());
}

void OpenBookWorker(void* arg) {
    auto* ctx = static_cast<OpenWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;
    try {

        const uint32_t token_now = st.reader.open_token.load();
        ESP_LOGI(TAG, "book_open worker start token=%u fmt=%d ", static_cast<unsigned>(ctx->token),
                 static_cast<int>(ctx->info.format));

        if (st.reader.session == nullptr) {
            ESP_LOGE(TAG, "book_open skip: session null");
        } else if (ctx->token != token_now) {
            ESP_LOGW(TAG, "book_open skip: token stale %u!=%u", static_cast<unsigned>(ctx->token),
                     static_cast<unsigned>(token_now));
        } else {
            SdAccessPermit permit;
            st.reader.session->SetOpenProgress(OnOpenProgress, reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->token)));
            ok = st.reader.session->Open(ctx->info);
            st.reader.session->SetOpenProgress(nullptr, nullptr);
            // 打开路径统一补旁路封面，供书架/详情/目录取用（失败不阻断阅读）
            if (ok && ctx->token == st.reader.open_token.load() &&
                (ctx->info.format == BookFormat::kEbook || ctx->info.format == BookFormat::kEpub)) {
                char sidecar[SD_FILE_PATH_CAPACITY];
                if (BookCoverSidecarPath(ctx->info.path.c_str(), sidecar, sizeof(sidecar)) && access(sidecar, R_OK) != 0) {
                    SaveBookCoverSidecar(ctx->info.path.c_str());
                }
            }
        }

    } catch (const std::bad_alloc&) {
        ok = false;
        if (st.reader.session) {
            st.reader.session->SetOpenProgress(nullptr, nullptr);
        }
        ESP_LOGE(TAG, "operation=open_book stage=execute result=no_memory");
    }

    ESP_LOGI(TAG, "book_open worker done ok=%d", ok ? 1 : 0);

    ctx->ok = ok;
    ctx->info = {};
    ScreenCompleteWork(ctx, true);
    vTaskDeleteWithCaps(nullptr);
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
    ReleaseReaderSessionForLeave();
    BackToLibraryAsync(nullptr);
}

void OpenDetailAsync(void* user_data) {
    CancelListCoverFill();
    CancelShelfDirectory();
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
    StartReadAsync(user_data);
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
        ESP_LOGW(TAG, "home continue CTA bad index=%d books=%u", index, static_cast<unsigned>(st.shelf.books.size()));
        return;
    }
    ESP_LOGI(TAG, "home continue CTA idx=%d ", index);
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    st.shelf.selected = index;
    if (lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index))) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "operation=navigate stage=enqueue result=full");
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
    }
}

void OnOpenShelfClicked(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    if (lv_async_call(OpenBookshelfAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "operation=navigate stage=enqueue result=full");
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
    }
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
    ESP_LOGI(TAG, "detail continue CTA sel=%d ", st.shelf.selected);
    if (lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(st.shelf.selected))) !=
        LV_RESULT_OK) {
        ESP_LOGW(TAG, "operation=navigate stage=enqueue result=full");
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
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
        s_window_modal.Bind(panel, nullptr, nullptr, UiModalPolicy::Locked);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, Book_ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 18, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(panel);
        lv_obj_center(panel);

        lv_obj_t* title = lv_label_create(panel);
        lv_label_set_text(title, LangString_BOOK_OPENING);
        lv_obj_set_style_text_font(title, Book_ListFont(), 0);
        lv_obj_set_style_text_color(title, UiColorForeground(), 0);
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
        lv_obj_set_style_bg_color(bar, UiColorBackground(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, UiColorForeground(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, UiColorForeground(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_bar = bar;

        lv_obj_t* pct = lv_label_create(panel);
        lv_obj_set_style_text_font(pct, Book_ListFont(), 0);
        lv_obj_set_style_text_color(pct, UiColorForeground(), 0);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_pct_lbl = pct;
    }

    lv_bar_set_value(st.reader.open_bar, percent, LV_ANIM_OFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(st.reader.open_pct_lbl, buf);
}

void StartOpenWorker(int book_index) try {
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        return;
    }
    if (st.reader.opening.load()) {
        ESP_LOGW(TAG, "StartOpenWorker ignored: already opening token=%u req_idx=%d ",
                 static_cast<unsigned>(st.reader.open_token.load()), book_index);
        return;
    }
    if (book_index < 0 || book_index >= static_cast<int>(st.shelf.books.size()) || !st.reader.session) {
        ESP_LOGE(TAG, "StartOpenWorker bad state idx=%d books=%u session=%d", book_index,
                 static_cast<unsigned>(st.shelf.books.size()), st.reader.session ? 1 : 0);
        Book_ShowMessage(st.reader.content, LangString_BOOK_OPEN_FAILED);
        return;
    }

    const uint32_t token = st.reader.open_token.fetch_add(1) + 1;
    st.reader.opening.store(true);
    st.reader.deferred_cleanup = false;
    st.reader.opening_format = st.shelf.books[static_cast<size_t>(book_index)].format;
    ShowOpenProgress(0);

    auto ctx = std::make_unique<OpenWorkerArg>();
    ctx->apply = ApplyOpenCompletion;
    ctx->token = token;
    ctx->info = st.shelf.books[static_cast<size_t>(book_index)];
    ESP_LOGI(TAG, "StartOpenWorker token=%u fmt=%d ", static_cast<unsigned>(token), static_cast<int>(ctx->info.format));
    BaseType_t created =
        FirmwareCreateTask(OpenBookWorker, "book_open", kOpenWorkerStack, ctx.get(), tskIDLE_PRIORITY + 2,
                                        nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "book_open task failed: int free=%u spiram=%u stack=%u", static_cast<unsigned>(free_int),
                 static_cast<unsigned>(free_spiram), static_cast<unsigned>(kOpenWorkerStack));
        st.reader.opening.store(false);
        Book_ShowMessage(st.reader.content, LangString_BOOK_OPEN_FAILED_TASK);
    } else {
        ctx.release();
    }
} catch (const std::bad_alloc&) {
    Book_State().reader.opening.store(false);
    ESP_LOGE(TAG, "operation=open_book stage=submit result=no_memory");
    Book_ShowMessage(Book_State().reader.content, LangString_BOOK_OPEN_FAILED_TASK);
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
    ESP_LOGI(TAG, "start read idx=%d ", index);
    CancelDetailCoverLoad();
    // 勿在此 Reset detail_cover：旧详情屏异步删除前仍引用封面缓冲
    ScreenLoadReplace(CreateReaderScreen(st.shelf.books[static_cast<size_t>(index)]));
}

void DispatchTapZoneAction(BookTapZoneAction action) {
    if (action == BookTapZoneAction::kPrev || action == BookTapZoneAction::kNext) {
        ReaderAutoTurnManualInput();
    }
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
        case BookTapZoneAction::kBookmarks:
            OpenReadBookmarks();
            break;
        case BookTapZoneAction::kFontSettings:
            OpenReadFontSettings();
            break;
        case BookTapZoneAction::kSettings:
            OpenReadSettings();
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

void BookApplyDeferredOpenResult() {
    if (!s_deferred_open) {
        return;
    }
    auto& state = Book_State();
    if (state.reader.standby_paused && !state.reader.deferred_cleanup && state.reader.content &&
        s_deferred_open->token == state.reader.open_token.load()) {
        return;
    }
    auto result = std::move(s_deferred_open);
    AsyncOpenDone(result.release());
}
