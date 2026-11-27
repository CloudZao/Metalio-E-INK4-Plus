
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "assets/lang_reader.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

void OnLayoutHintTick(lv_timer_t* /*t*/) {
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        return;
    }
    if (st.reader.page_label == nullptr || !lv_obj_is_valid(st.reader.page_label)) {
        StopLayoutHintTimer();
        return;
    }
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges() || ReaderPageWorkPending() ||
        s_reader_paint.seq.load() != s_reader_paint.done.load()) {
        return;
    }
    if (EpdGetStats().busy) {
        return;
    }
    // 沉浸阅读时只接收状态，底栏随翻页绘制，避免周期更新另占一次刷屏。
    if (IsLayoutHintBusy()) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kReading) {
        if (esp_timer_get_time() < st.reader.layout_hint_done_until_us) {
            return;
        }
        st.reader.layout_hint_done_until_us = 0;
        StopLayoutHintTimer();
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

void OnLayoutDebounceTimer(lv_timer_t* t) {
    auto& st = Book_State();
    if (st.reader.layout_debounce_timer == t) {
        st.reader.layout_debounce_timer = nullptr;
    }
    // 与 ScreenPaintCoalesce 同形：手指未抬 / 边沿未交完则再等一拍，避免连点中途开排版
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges()) {
        st.reader.layout_debounce_timer = lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
        if (st.reader.layout_debounce_timer != nullptr) {
            lv_timer_set_repeat_count(st.reader.layout_debounce_timer, 1);
        }
        return;
    }
    const bool reload = st.reader.layout_debounce_reload_font;
    st.reader.layout_debounce_reload_font = false;
    ApplyReaderLayoutLive(reload);
}

void ApplyReaderLayoutLive(bool reload_font) {
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        st.reader.layout_again = true;
        st.reader.layout_reload_font = st.reader.layout_reload_font || reload_font || BookFontReloadBusy();
        return;
    }
    if (st.reader.prefs_reopen_pending || st.reader.opening.load() || !st.reader.session ||
        !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.viewport_w <= 0 || st.reader.viewport_h <= 0) {
        return;
    }
    // 先停止旧编排，再应用新参数，避免两个后台任务并行或共享 font_ 被释放。
    if (ReaderWorkersBusy()) {
        CancelReaderPageWork();
        st.reader.layout_again = true;
        st.reader.layout_reload_font = st.reader.layout_reload_font || reload_font || BookFontReloadBusy();

        st.reader.session->AbortChapterLayout();
        st.reader.page_image_abort.store(true);
        return;
    }

    CancelReaderPageWork();
    if (reload_font) {
        if (!StartBookFontReload()) {
            ScreenShowToast(LangString_TTF_APPLY_FAILED);
        }
        return;
    }
    const bool images_changed = st.reader.session->Style().inline_images != (BookReaderPrefsInlineImages() != 0);
    BindSessionLayoutPrefs(*st.reader.session);
    const auto area = ReadContentArea();
    const bool viewport_changed = lv_area_get_width(&area) != st.reader.session->ViewportWidth() ||
                                  lv_area_get_height(&area) != st.reader.session->ViewportHeight();
    // 漫画也需要随底栏显隐及布局变化更新视口和页图尺寸。
    if (!images_changed && !viewport_changed && DeferComicTextSetting()) {
        return;
    }
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.reader.session->SetViewport(st.reader.viewport_w, st.reader.viewport_h);

    BookSession::LayoutAnchor anchor = st.reader.session->CaptureLayoutAnchor();
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

void FlushLayoutDebounce() {
    auto& st = Book_State();
    if (st.reader.layout_debounce_timer == nullptr) {
        return;
    }
    const bool reload = st.reader.layout_debounce_reload_font;
    CancelLayoutDebounce();
    ApplyReaderLayoutLive(reload);
}

void ScheduleLayoutApply(bool reload_font) {
    auto& st = Book_State();
    if (st.reader.prefs_reopen_pending || st.reader.opening.load() || !st.reader.session ||
        !st.reader.session->IsOpen()) {
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

bool IsLayoutHintBusy() {
    auto& st = Book_State();
    if (st.reader.layout_busy.load()) {
        return true;
    }
    return st.reader.session && st.reader.session->IsOpen() && st.reader.session->LiveRelayoutPending() &&
           st.reader.session->Error() == BookReadError::kNone;
}

void MarkLayoutHintBusy() {
    auto& st = Book_State();
    st.reader.layout_hint_done_until_us = 0;
    EnsureLayoutHintTimer();
    if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
        UpdateReadFooter();
    }
}

void MarkLayoutHintDone() {
    auto& st = Book_State();
    st.reader.layout_hint_done_until_us = esp_timer_get_time() + kLayoutHintDoneUs;
    EnsureLayoutHintTimer();
    if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
        UpdateReadFooter();
    }
}

void EnsureLayoutHintTimer() {
    auto& st = Book_State();
    if (st.reader.layout_hint_timer != nullptr) {
        return;
    }
    st.reader.layout_hint_timer = lv_timer_create(OnLayoutHintTick, kLayoutHintTickMs, nullptr);
}

void StopLayoutHintTimer() {
    auto& st = Book_State();
    if (st.reader.layout_hint_timer != nullptr) {
        lv_timer_del(st.reader.layout_hint_timer);
        st.reader.layout_hint_timer = nullptr;
    }
}
