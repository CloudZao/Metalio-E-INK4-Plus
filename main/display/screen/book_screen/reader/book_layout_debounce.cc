#pragma GCC optimize("O1")

#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "lv_adapter_display.h"
#include "reader/reader.h"
#include "screen_common.h"

void ApplyReaderLayoutLive(bool reload_font) {
    auto& st = Book_State();
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.viewport_w <= 0 || st.reader.viewport_h <= 0) {
        return;
    }
    const bool layout_running = st.reader.layout_busy.load();
    // 换字库与 worker 抢 font_：只记 again；边距/行距仍即时刷预览
    if (layout_running && reload_font) {
        st.reader.layout_again = true;
        st.reader.layout_reload_font = true;
        return;
    }
    if (layout_running) {
        st.reader.layout_again = true;
    }
    // 章节页表后台正用 font_ 计宽：禁止 ReleaseBookFont，否则 InstrFetchProhibited
    if (reload_font && st.reader.chapter_pages_busy.load()) {
        st.reader.layout_again = true;
        st.reader.layout_reload_font = true;
        st.reader.chapter_pages_again = true;
        st.reader.session->InvalidateChapterPageTable();
        return;
    }

    s_reader_page_delta.store(0, std::memory_order_release);
    if (reload_font) {
        ReleaseBookFont();
        EnsureBookFont();
    }
    BindSessionLayoutPrefs(*st.reader.session);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.reader.session->SetViewport(st.reader.viewport_w, st.reader.viewport_h);

    reader::BookSession::LayoutAnchor anchor = st.reader.session->CaptureLayoutAnchor();
    if (!st.reader.session->BeginLiveRelayout(&anchor)) {
        ESP_LOGW(TAG, "live relayout failed, fallback reopen");
        st.reader.settings_resume_after_open = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
        return;
    }

    const bool keep_sheet = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.reader.session->NeedsTxtIndexRebuild() && st.reader.session->LiveRelayoutPending()) {
        st.reader.layout_anchor = anchor;
        if (layout_running) {
            // worker 结束后用最新锚点/参数再全量扫
            st.reader.layout_again = true;
        } else {
            st.reader.layout_again = false;
            st.reader.layout_reload_font = false;
            StartLayoutWorker();
        }
    } else if (st.reader.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
}

void CancelLayoutDebounce() {
    auto& st = Book_State();
    if (st.reader.layout_debounce_timer != nullptr) {
        lv_timer_del(st.reader.layout_debounce_timer);
        st.reader.layout_debounce_timer = nullptr;
    }
    st.reader.layout_debounce_reload_font = false;
}

bool IsLayoutHintBusy() {
    auto& st = Book_State();
    if (st.reader.chapter_pages_busy.load() || st.reader.layout_busy.load()) {
        return true;
    }
    return st.reader.session && st.reader.session->IsOpen() && st.reader.session->LiveRelayoutPending();
}

void StopLayoutHintTimer() {
    auto& st = Book_State();
    if (st.reader.layout_hint_timer != nullptr) {
        lv_timer_del(st.reader.layout_hint_timer);
        st.reader.layout_hint_timer = nullptr;
    }
}

void OnLayoutHintTick(lv_timer_t* /*t*/) {
    auto& st = Book_State();
    if (st.reader.page_label == nullptr || !lv_obj_is_valid(st.reader.page_label)) {
        StopLayoutHintTimer();
        return;
    }
    if (IsLayoutHintBusy()) {
        UpdateReadFooter();
        return;
    }
    if (esp_timer_get_time() < st.reader.layout_hint_done_until_us) {
        UpdateReadFooter();
        return;
    }
    st.reader.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void EnsureLayoutHintTimer() {
    auto& st = Book_State();
    if (st.reader.layout_hint_timer != nullptr) {
        return;
    }
    st.reader.layout_hint_timer = lv_timer_create(OnLayoutHintTick, kLayoutHintTickMs, nullptr);
}

void MarkLayoutHintBusy() {
    auto& st = Book_State();
    st.reader.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void MarkLayoutHintDone() {
    auto& st = Book_State();
    st.reader.layout_hint_done_until_us = esp_timer_get_time() + kLayoutHintDoneUs;
    EnsureLayoutHintTimer();
    UpdateReadFooter();
}

void OnLayoutDebounceTimer(lv_timer_t* t) {
    auto& st = Book_State();
    if (st.reader.layout_debounce_timer == t) {
        st.reader.layout_debounce_timer = nullptr;
    }
    // 与 ScreenPaintCoalesce 同形：手指未抬 / 边沿未交完则再等一拍，避免连点中途开排版
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges()) {
        st.reader.layout_debounce_timer =
            lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
        if (st.reader.layout_debounce_timer != nullptr) {
            lv_timer_set_repeat_count(st.reader.layout_debounce_timer, 1);
        }
        return;
    }
    const bool reload = st.reader.layout_debounce_reload_font;
    st.reader.layout_debounce_reload_font = false;
    ApplyReaderLayoutLive(reload);
}

void ScheduleLayoutApply(bool reload_font) {
    auto& st = Book_State();
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    // prefs/卡文案已由调用方写好；抬起且停顿后再排版（合并为最后一档）
    st.reader.layout_debounce_reload_font = st.reader.layout_debounce_reload_font || reload_font;
    if (st.reader.layout_debounce_timer != nullptr) {
        lv_timer_reset(st.reader.layout_debounce_timer);
        return;
    }
    st.reader.layout_debounce_timer = lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
    if (st.reader.layout_debounce_timer != nullptr) {
        lv_timer_set_repeat_count(st.reader.layout_debounce_timer, 1);
    }
}

void FlushLayoutDebounce() {
    auto& st = Book_State();
    if (st.reader.layout_debounce_timer == nullptr) {
        return;
    }
    const bool reload = st.reader.layout_debounce_reload_font;
    CancelLayoutDebounce();
    ApplyReaderLayoutLive(reload);
}

