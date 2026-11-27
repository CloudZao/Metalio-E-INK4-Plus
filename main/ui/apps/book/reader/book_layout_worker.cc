#include <atomic>
#include <cstdint>
#include <memory>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "infra/tasks/task_create.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "assets/lang_reader.h"
#include "boards/common/storage/sd_access.h"
#include "business/reader/reader.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_open_worker.h"
#include "ui/apps/book/reader/book_page_image.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "business/reader/book_tap_zones.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/apps/standby/standby_screen.h"

struct BookLayoutWorkerLayoutCompletion {
    uint32_t token = 0;              // 完成任务的代次
    bool ok = false;                 // 编排结果
    std::atomic<bool> ready{false};  // 单个在途任务的完成槽，由 UI 消费
};

static BookLayoutWorkerLayoutCompletion layout_done;

struct LayoutWorkerArg {
    uint32_t token = 0;
    uint32_t generation = 0; // 提交时固定，防止启动前的取消被任务漏过
    BookSession::LayoutAnchor anchor{};
};

static void OnLayoutDone(uint32_t token, bool ok) {
    auto& st = Book_State();

    const bool mine = (token == st.reader.layout_token.load());
    // 无论 token 是否作废，本 worker 已结束，必须清 busy（离开时不再提前清）
    st.reader.layout_busy.store(false);
    if (st.reader.standby_paused) {
        return;
    }
    // 离开正文时 layout worker 仍在跑：结束后再关 session
    if (st.reader.deferred_cleanup && !st.reader.opening.load() && !st.reader.layout_busy.load() &&
        !st.reader.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (st.reader.prefs_reopen_pending || !mine || st.reader.content == nullptr || !st.reader.session) {
        return;
    }
    if (st.reader.layout_again) {
        return;
    }
    if (!ok) {
        if (st.reader.session->Error() != BookReadError::kCancelled) {
            ESP_LOGW(TAG, "layout worker failed error=%d", static_cast<int>(st.reader.session->Error()));
        }
        // 后台索引失败时保留当前可读页，不自动重开并重复扫描整本书。
        st.reader.layout_hint_done_until_us = 0;
        StopLayoutHintTimer();
        UpdateReadFooter();
        RequestRenderReaderPage();
        return;
    }

    InvalidateReaderPagePrefetch();
    st.reader.session->CommitLiveRelayout(st.reader.layout_anchor);
    const bool keep_sheet = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    RequestRenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    }

    if (!IsLayoutHintBusy()) {
        MarkLayoutHintDone();
    }
}

void LayoutWorker(void* arg) {
    auto* ctx = static_cast<LayoutWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;
    try {
        if (st.reader.session && ctx->token == st.reader.layout_token.load()) {
            SdAccessPermit permit;
            ok = st.reader.session->FinishLiveRelayout(ctx->anchor, ctx->generation);
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=relayout stage=execute result=no_memory");
    }
    layout_done.token = ctx->token;
    layout_done.ok = ok;
    delete ctx;
    layout_done.ready.store(true, std::memory_order_release);
    BookRequestUiWork();
    vTaskDeleteWithCaps(nullptr);
}

static void OnPrefsReopenReady(lv_timer_t* /*timer*/) {
    auto& st = Book_State();
    if (st.reader.deferred_cleanup || st.reader.content == nullptr || st.reader.read_scr == nullptr) {
        CancelReaderPrefsReopen();
        return;
    }
    if (st.reader.standby_paused || StandbyPresentation::IsActive() || ReaderWorkersBusy()) {
        return;
    }
    CancelReaderPrefsReopen();
    ReopenReaderAfterPrefsChange();
}

void StartLayoutWorker() {
    auto& st = Book_State();
    if (ReaderPageWorkPending() && (!st.reader.session ||
        (!st.reader.session->HasCurrentPagePreview() && !st.reader.session->StructurePending()))) {
        return;
    }
    if (ReaderWorkersBusy() || st.reader.standby_paused || StandbyPresentation::IsActive()) {
        return;
    }
    if (st.reader.prefs_reopen_pending || !st.reader.session) {
        return;
    }
    const uint32_t token = st.reader.layout_token.fetch_add(1) + 1;
    st.reader.layout_busy.store(true);
    MarkLayoutHintBusy();
    auto* ctx = new (std::nothrow) LayoutWorkerArg{};
    if (!ctx) {
        st.reader.layout_busy.store(false);
        ESP_LOGE(TAG, "operation=relayout stage=submit result=no_memory");
        return;
    }
    ctx->token = token;
    ctx->generation = st.reader.session->AbortChapterLayout();
    ctx->anchor = st.reader.layout_anchor;
    BaseType_t created =
        FirmwareCreateTask(LayoutWorker, "book_layout", kOpenWorkerStack, ctx, tskIDLE_PRIORITY + 1,
                                        nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "book_layout task failed");
        delete ctx;
        st.reader.layout_busy.store(false);
        st.reader.session->ClearLivePreview();
        st.reader.settings_resume_after_open = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
    }
}

void PollReaderLayoutWork() {
    if (layout_done.ready.exchange(false, std::memory_order_acquire)) {
        OnLayoutDone(layout_done.token, layout_done.ok);
    }
}

void ReopenReaderAfterPrefsChange() {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    // A token only rejects callbacks; it cannot stop a worker already reading session/font.
    // Keep the old objects and real busy flags until every worker has finished using them.
    if (st.reader.deferred_cleanup || st.reader.content == nullptr || st.reader.read_scr == nullptr) {
        CancelReaderPrefsReopen();
        return;
    }
    if (StandbyPresentation::IsActive() || ReaderWorkersBusy()) {
        if (!st.reader.prefs_reopen_pending) {
            st.reader.prefs_reopen_timer = lv_timer_create(OnPrefsReopenReady, 50, nullptr);
            if (st.reader.prefs_reopen_timer == nullptr) {
                ESP_LOGE(TAG, "prefs reopen: no wait timer; keep session/font alive");
                return;
            }
            st.reader.prefs_reopen_pending = true;
            st.reader.layout_token.fetch_add(1);
            CancelLayoutDebounce();
            CancelReaderPageWork();
            CancelPageImageLoad();
            if (st.reader.session) {
                st.reader.session->AbortChapterLayout();
            }
            ESP_LOGI(TAG, "prefs reopen deferred: layout=%d image=%d", st.reader.layout_busy.load(),
                     st.reader.page_image_busy.load());
        }
        return;
    }
    CancelReaderPrefsReopen();
    CancelLayoutDebounce();
    st.reader.layout_token.fetch_add(1);
    st.reader.layout_again = false;
    st.reader.layout_reload_font = false;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    SetReadStatusVisible(false);
    SetReadSettingsSheetVisible(false);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    if (st.reader.viewport_w <= 0 || st.reader.viewport_h <= 0) {
        ESP_LOGE(TAG, "reopen prefs: invalid viewport %d×%d", static_cast<int>(st.reader.viewport_w),
                 static_cast<int>(st.reader.viewport_h));
        st.reader.settings_resume_after_open = false;
        return;
    }
    const lv_coord_t vw = st.reader.viewport_w;
    const lv_coord_t vh = st.reader.viewport_h;

    if (!RetireReaderSession()) {
        st.reader.prefs_reopen_timer = lv_timer_create(OnPrefsReopenReady, 50, nullptr);
        st.reader.prefs_reopen_pending = st.reader.prefs_reopen_timer != nullptr;
        return;
    }
    if (!ReloadBookFont()) {
        ScreenShowToast(LangString_TTF_APPLY_FAILED);
    }
    st.reader.session = std::make_unique<BookSession>();
    BindSessionLayoutPrefs(*st.reader.session);
    st.reader.session->SetViewport(vw, vh);
    StartOpenWorker(st.shelf.selected);
}

void CancelReaderPrefsReopen() {
    auto& st = Book_State();
    st.reader.prefs_reopen_pending = false;
    if (st.reader.prefs_reopen_timer != nullptr) {
        lv_timer_del(st.reader.prefs_reopen_timer);
        st.reader.prefs_reopen_timer = nullptr;
    }
}

void OnTocRowClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (!RequestReaderNavigation(BookSession::NavigationKind::kToc, index)) {
        return;
    }
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    RenderReaderPage();
}

void OnReadContentClicked(lv_event_t* e) {
    auto& st = Book_State();
    if (st.reader.content == nullptr || lv_event_get_target(e) != st.reader.content) {
        return;
    }
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        // 设置卡片打开时点正文只收起，不走分区动作
        HideReadChrome();
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        // 目录整页：页内触摸不退出、不翻页；仅盖板 vk 返回
        return;
    }
    lv_point_t pt = {};
    if (!ReadEventPoint(e, &pt)) {
        return;
    }
    const int cell = BookTapZonesHit(pt.x, pt.y, LV_HOR_RES, LV_VER_RES);
    if (cell < 0) {
        return;
    }
    DispatchTapZoneAction(BookTapZonesActionAt(cell));
}
