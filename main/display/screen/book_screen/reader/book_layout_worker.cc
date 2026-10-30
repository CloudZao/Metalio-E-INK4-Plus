#pragma GCC optimize("O1")

#include "book_screen/reader/book_layout_worker.h"
#include "assets/lang_reader.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/reader/book_page_image.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <esp_log.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"
#include "sd_access.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"

namespace {
struct LayoutCompletion {
    uint32_t token = 0; // 完成任务的代次
    bool ok = false; // 编排结果
    std::atomic<bool> ready{false}; // 单个在途任务的完成槽，由 UI 消费
};
LayoutCompletion layout_done, chapter_pages_done;
}

struct LayoutWorkerArg {
    uint32_t token = 0;
    reader::BookSession::LayoutAnchor anchor{};
};

static void OnLayoutDone(uint32_t token, bool ok) {
    auto& st = Book_State();

    const bool mine = (token == st.reader.layout_token.load());
    // 无论 token 是否作废，本 worker 已结束，必须清 busy（离开时不再提前清）
    st.reader.layout_busy.store(false);
    if (st.reader.standby_paused) return;
    // 离开正文时 layout worker 仍在跑：结束后再关 session
    if (st.reader.deferred_cleanup && !st.reader.opening.load() && !st.reader.layout_busy.load() &&
        !st.reader.chapter_pages_busy.load() && !st.reader.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (st.reader.prefs_reopen_pending || !mine || st.reader.content == nullptr || !st.reader.session) {
        return;
    }
    if (st.reader.layout_again) return;
    if (!ok) {
        if (st.reader.session->Error() != reader::BookReadError::kCancelled)
            ESP_LOGW(TAG, "layout worker failed error=%d", static_cast<int>(st.reader.session->Error()));
        // 后台索引失败时保留当前可读页，不自动重开并重复扫描整本书。
        st.reader.layout_hint_done_until_us = 0;
        StopLayoutHintTimer();
        UpdateReadFooter();
        RequestRenderReaderPage();
        return;
    }

    st.reader.session->CommitLiveRelayout(st.reader.layout_anchor);
    const bool keep_sheet = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    RequestRenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    // 正文绘制请求完成后再启动整书页数统计，避免争用字体任务和 SD。
    if ((keep_sheet || !st.reader.session->NeedsChapterPageRebuild()) && !IsLayoutHintBusy()) {
        MarkLayoutHintDone();
    }
}

void LayoutWorker(void* arg) {
    auto* ctx = static_cast<LayoutWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;
    if (st.reader.session && ctx->token == st.reader.layout_token.load()) {
        sd_access::Permit permit;
        ok = st.reader.session->FinishLiveRelayout(ctx->anchor);
    }
    layout_done.token = ctx->token;
    layout_done.ok = ok;
    delete ctx;
    layout_done.ready.store(true, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

static void OnChapterPagesDone(uint32_t token, bool ok) {
    auto& st = Book_State();

    const bool mine = (token == st.reader.chapter_pages_token.load());
    st.reader.chapter_pages_busy.store(false);
    if (st.reader.standby_paused) return;

    if (st.reader.deferred_cleanup && !st.reader.opening.load() && !st.reader.layout_busy.load() &&
        !st.reader.chapter_pages_busy.load() && !st.reader.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (st.reader.prefs_reopen_pending || !mine || st.reader.content == nullptr || !st.reader.session) {
        return;
    }
    st.reader.session->PollLayoutProgress();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }
    // 换字号曾因页表 worker 占用 font 而 defer：worker 结束后再落地
    if (st.reader.layout_again) return;
    StartPageImageWorker();
    if (!ok) {
        st.reader.layout_hint_done_until_us = 0;
        StopLayoutHintTimer();
        UpdateReadFooter();
        return;
    }
    if (st.reader.session->NeedsChapterPageRebuild()) return;
    MarkLayoutHintDone();
}

void ChapterPageWorker(void* arg) {
    auto* ctx = static_cast<uint32_t*>(arg);
    const uint32_t token = *ctx;
    delete ctx;
    auto& st = Book_State();
    bool ok = false;
    if (st.reader.session && token == st.reader.chapter_pages_token.load()) {
        sd_access::Permit permit;
        ok = st.reader.session->FinishChapterPageTable();
    }
    chapter_pages_done.token = token;
    chapter_pages_done.ok = ok;
    chapter_pages_done.ready.store(true, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

void PollReaderLayoutWork() {
    if (layout_done.ready.exchange(false, std::memory_order_acquire))
        OnLayoutDone(layout_done.token, layout_done.ok);
    if (chapter_pages_done.ready.exchange(false, std::memory_order_acquire))
        OnChapterPagesDone(chapter_pages_done.token, chapter_pages_done.ok);
}

void StartChapterPageWorker() {
    auto& st = Book_State();
    if (st.reader.deferred_cleanup || !st.reader.read_scr || !st.reader.content) return;
    if (st.reader.standby_paused || StandbyScreen::IsActive()) return;
    if (st.reader.opening.load() || st.reader.layout_busy.load() || st.reader.page_image_busy.load()) return;
    if (st.reader.prefs_reopen_pending || !st.reader.session || !st.reader.session->NeedsChapterPageRebuild()) {
        return;
    }
    if (st.reader.session->LiveRelayoutPending()) return;
    if (st.reader.chapter_pages_busy.load()) return;
    const uint32_t token = st.reader.chapter_pages_token.fetch_add(1) + 1;
    st.reader.chapter_pages_busy.store(true);
    MarkLayoutHintBusy();
    auto* ctx = new uint32_t(token);
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        ChapterPageWorker, "book_cpages", kOpenWorkerStack, ctx, tskIDLE_PRIORITY + 1, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "book_cpages task failed");
        delete ctx;
        st.reader.chapter_pages_busy.store(false);
        MarkLayoutHintDone();
    }
}

void StartLayoutWorker() {
    if (ReaderPageWorkPending()) return;
    auto& st = Book_State();
    if (st.reader.standby_paused || StandbyScreen::IsActive()) return;
    if (st.reader.opening.load() || st.reader.chapter_pages_busy.load() || st.reader.page_image_busy.load()) return;
    if (st.reader.prefs_reopen_pending || st.reader.layout_busy.load() || !st.reader.session) {
        return;
    }
    const uint32_t token = st.reader.layout_token.fetch_add(1) + 1;
    st.reader.layout_busy.store(true);
    MarkLayoutHintBusy();
    auto* ctx = new LayoutWorkerArg{};
    ctx->token = token;
    ctx->anchor = st.reader.layout_anchor;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        LayoutWorker, "book_layout", kOpenWorkerStack, ctx, tskIDLE_PRIORITY + 1, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "book_layout task failed");
        delete ctx;
        st.reader.layout_busy.store(false);
        st.reader.session->ClearLivePreview();
        st.reader.settings_resume_after_open = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
    }
}

void CancelReaderPrefsReopen() {
    auto& st = Book_State();
    st.reader.prefs_reopen_pending = false;
    if (st.reader.prefs_reopen_timer != nullptr) {
        lv_timer_del(st.reader.prefs_reopen_timer);
        st.reader.prefs_reopen_timer = nullptr;
    }
}

static void OnPrefsReopenReady(lv_timer_t* /*timer*/) {
    auto& st = Book_State();
    if (st.reader.deferred_cleanup || st.reader.content == nullptr || st.reader.read_scr == nullptr) {
        CancelReaderPrefsReopen();
        return;
    }
    if (st.reader.standby_paused || StandbyScreen::IsActive() || ReaderWorkersBusy()) {
        return;
    }
    CancelReaderPrefsReopen();
    ReopenReaderAfterPrefsChange();
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
    if (StandbyScreen::IsActive() || ReaderWorkersBusy()) {
        if (!st.reader.prefs_reopen_pending) {
            st.reader.prefs_reopen_timer = lv_timer_create(OnPrefsReopenReady, 50, nullptr);
            if (st.reader.prefs_reopen_timer == nullptr) {
                ESP_LOGE(TAG, "prefs reopen: no wait timer; keep session/font alive");
                return;
            }
            st.reader.prefs_reopen_pending = true;
            st.reader.layout_token.fetch_add(1);
            st.reader.chapter_pages_token.fetch_add(1);
            CancelLayoutDebounce();
            CancelReaderPageWork();
            CancelPageImageLoad();
            if (st.reader.session) {
                st.reader.session->AbortTxtPaginate();
                st.reader.session->AbortChapterPageTable();
            }
            ESP_LOGI(TAG, "prefs reopen deferred: layout=%d chapters=%d image=%d",
                     st.reader.layout_busy.load(), st.reader.chapter_pages_busy.load(),
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
    if (!ReloadBookFont()) ScreenShowToast(Lang::Strings::TTF_APPLY_FAILED);
    st.reader.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.reader.session);
    st.reader.session->SetViewport(vw, vh);
    StartOpenWorker(st.shelf.selected);
}

void OnTocRowClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (!RequestReaderNavigation(reader::BookSession::NavigationKind::kToc, index)) {
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

