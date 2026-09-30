#pragma GCC optimize("O1")

#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <esp_log.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"
#include "screen_common.h"

struct LayoutDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct LayoutWorkerArg {
    uint32_t token = 0;
    reader::BookSession::LayoutAnchor anchor{};
};

void AsyncLayoutDone(void* user_data) {
    auto* msg = static_cast<LayoutDoneMsg*>(user_data);
    auto& st = Book_State();
    const uint32_t token = msg->token;
    const bool ok = msg->ok;
    delete msg;

    const bool mine = (token == st.reader.layout_token.load());
    // 无论 token 是否作废，本 worker 已结束，必须清 busy（离开时不再提前清）
    st.reader.layout_busy.store(false);
    // 离开正文时 layout worker 仍在跑：结束后再关 session
    if (st.reader.deferred_cleanup && !st.reader.opening.load() && !st.reader.layout_busy.load() &&
        !st.reader.chapter_pages_busy.load() && !st.reader.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.reader.content == nullptr || !st.reader.session) {
        return;
    }
    if (!ok) {
        ESP_LOGW(TAG, "layout worker failed");
        if (st.reader.session &&
            (st.reader.session->LiveRelayoutPending() || st.reader.session->NeedsTxtIndexRebuild())) {
            st.reader.session->ClearLivePreview();
            st.reader.settings_resume_after_open = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
            ReopenReaderAfterPrefsChange();
        } else if (st.reader.session) {
            UpdateReadFooter();
        }
        return;
    }

    const bool keep_sheet = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.reader.layout_again) {
        const bool reload = st.reader.layout_reload_font;
        st.reader.layout_again = false;
        st.reader.layout_reload_font = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (!IsLayoutHintBusy()) {
        MarkLayoutHintDone();
    }
}

void LayoutWorker(void* arg) {
    auto* ctx = static_cast<LayoutWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;
    if (st.reader.session && ctx->token == st.reader.layout_token.load()) {
        ok = st.reader.session->FinishTxtRelayout(ctx->anchor);
    }
    auto* done = new LayoutDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncLayoutDone, done)) {
        delete done;
        st.reader.layout_busy.store(false);
    }
    delete ctx;
    vTaskDelete(nullptr);
}

struct ChapterPagesDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

void AsyncChapterPagesDone(void* user_data) {
    auto* msg = static_cast<ChapterPagesDoneMsg*>(user_data);
    auto& st = Book_State();
    const uint32_t token = msg->token;
    delete msg;

    const bool mine = (token == st.reader.chapter_pages_token.load());
    st.reader.chapter_pages_busy.store(false);

    if (st.reader.deferred_cleanup && !st.reader.opening.load() && !st.reader.layout_busy.load() &&
        !st.reader.chapter_pages_busy.load() && !st.reader.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.reader.content == nullptr || !st.reader.session) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }
    // 换字号曾因页表 worker 占用 font 而 defer：worker 结束后再落地
    if (st.reader.layout_again) {
        const bool reload = st.reader.layout_reload_font;
        st.reader.layout_again = false;
        st.reader.layout_reload_font = false;
        st.reader.chapter_pages_again = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (st.reader.chapter_pages_again || st.reader.session->NeedsChapterPageRebuild()) {
        st.reader.chapter_pages_again = false;
        StartChapterPageWorker(); // 设置卡内会直接 return，关卡后再建
        return;
    }
    MarkLayoutHintDone();
}

void ChapterPageWorker(void* arg) {
    auto* ctx = static_cast<uint32_t*>(arg);
    const uint32_t token = *ctx;
    delete ctx;
    auto& st = Book_State();
    bool ok = false;
    if (st.reader.session && token == st.reader.chapter_pages_token.load()) {
        ok = st.reader.session->FinishChapterPageTable();
    }
    auto* done = new ChapterPagesDoneMsg{};
    done->token = token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncChapterPagesDone, done)) {
        delete done;
        st.reader.chapter_pages_busy.store(false);
    }
    vTaskDelete(nullptr);
}

void StartChapterPageWorker() {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->NeedsChapterPageRebuild()) {
        return;
    }
    // 设置浮层只预览当前章；Needs 仍真，关卡 HideReadChrome 再调本函数
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        return;
    }
    if (st.reader.chapter_pages_busy.load()) {
        st.reader.chapter_pages_again = true;
        return;
    }
    const uint32_t token = st.reader.chapter_pages_token.fetch_add(1) + 1;
    st.reader.chapter_pages_busy.store(true);
    st.reader.chapter_pages_again = false;
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
    auto& st = Book_State();
    if (st.reader.layout_busy.load() || !st.reader.session) {
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

void ReopenReaderAfterPrefsChange() {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    st.reader.layout_token.fetch_add(1);
    st.reader.layout_busy.store(false);
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

    s_reader_page_delta.store(0, std::memory_order_release);
    EndReadingTimeTracking();
    st.reader.session.reset();
    ReleaseBookFont();
    EnsureBookFont();
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
    if (!st.reader.session->GoToToc(index)) {
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

