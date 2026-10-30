#include "screen_frontlight.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/book_nav.h"
#include "book_screen/settings/book_settings_sheet.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <esp_log.h>

#include "lv_adapter_display.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "reader/reader.h"

void ApplyReaderOrient(int orient) {
    if (!DisplayUiSetOrient(orient)) {
        return;
    }
    ApplyReaderChromeSize();
    ShowReaderPageLoading();
    RebuildSettingsSheet();
    ReaderAutoTurnManualInput();
    ApplyReaderLayoutLive(false);
    EpdCancelPageTurn();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestFullOnNextCommit();
    }
}

lv_coord_t ReaderFooterHeight() {
    return static_cast<lv_coord_t>(Book_ItemFont()->line_height + 2 * UiSy(2));
}

namespace {
lv_coord_t ResizeReaderContainers(bool show_footer) {
    auto& reader = Book_State().reader;
    const lv_coord_t footer_h = show_footer ? ReaderFooterHeight() : 0;
    const lv_coord_t body_h = LV_VER_RES - footer_h;
    if (reader.content && lv_obj_is_valid(reader.content)) {
        lv_obj_set_size(reader.content, LV_HOR_RES, body_h);
        lv_obj_align(reader.content, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_remove_flag(reader.content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    if (reader.footer_host && lv_obj_is_valid(reader.footer_host)) {
        lv_obj_set_size(reader.footer_host, LV_HOR_RES, footer_h);
        lv_obj_align(reader.footer_host, LV_ALIGN_TOP_LEFT, 0, body_h);
        lv_obj_set_flag(reader.footer_host, LV_OBJ_FLAG_HIDDEN, !show_footer);
    }
    if (reader.page_label && lv_obj_is_valid(reader.page_label)) {
        lv_obj_set_width(reader.page_label, LV_HOR_RES - UiSx(16));
        lv_obj_set_flag(reader.page_label, LV_OBJ_FLAG_HIDDEN, !show_footer);
        lv_obj_center(reader.page_label);
    }
    return body_h;
}
}

bool IsReadOverlayChrome(BookUiState::ReadChrome c) {
    using C = BookUiState::ReadChrome;
    return c == C::kToc || c == C::kSettings;
}

void SyncReaderAaForImmersion(bool text_visible) {
    BookReaderPrefsSyncGlyphAa(text_visible && !ReaderPageImagesHint() ? 1 : 0);
}

void BindSessionLayoutPrefs(reader::BookSession& session) {
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    session.SetFont(BookLayoutFont());
    session.SetVerticalWriting(BookReaderPrefsWritingMode() == kBookReaderWritingVertical &&
                               !ReaderPageImagesHint());
    session.SetLineSpacing(BookReaderPrefsLineSpacingPercent(),
                           static_cast<lv_coord_t>(BookReaderPrefsParaGap()));
}

lv_area_t ReadContentArea() {
    BookReaderPrefsEnsureLoaded();
    const bool page_images = ReaderPageImagesHint();
    const bool vertical = !page_images && BookReaderPrefsWritingMode() == kBookReaderWritingVertical;
    const int extra_side = vertical ? kReaderVerticalSideInset : 0;
    const int left = BookReaderPrefsMarginLeft() + (vertical &&
        BookReaderPrefsTextDirection() != kBookReaderTextRightToLeft ? extra_side : 0);
    const int top = BookReaderPrefsMarginTop();
    const int width = std::max<int>(40, LV_HOR_RES - BookReaderPrefsMarginLeft() -
        BookReaderPrefsMarginRight() - extra_side);
    const int height = std::max<int>(40, LV_VER_RES -
        (BookReaderPrefsHideProgress() ? 0 : ReaderFooterHeight()) - top - BookReaderPrefsMarginBottom());
    return {left, top, left + width - 1, top + height - 1};
}

// 目录/设置等列表：固定用书库边距，避免阅读页边距档位挤乱菜单行宽
void ApplyOverlayListContentPads(lv_coord_t top_pad) {
    auto& st = Book_State();
    if (st.reader.content == nullptr) {
        return;
    }
    lv_obj_set_style_pad_top(st.reader.content, top_pad, 0);
    lv_obj_set_style_pad_bottom(st.reader.content, 0, 0);
    lv_obj_set_style_pad_left(st.reader.content, kListPad, 0);
    lv_obj_set_style_pad_right(st.reader.content, kListPad, 0);
}

// 是否整页图模式（未 Open 时用 Peek，便于建屏设 viewport）
bool ReaderPageImagesHint() {
    auto& st = Book_State();
    if (st.reader.session && st.reader.session->IsOpen()) {
        return st.reader.session->IsPageImagesMode();
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return false;
    }
    const reader::BookInfo& info = st.shelf.books[static_cast<size_t>(st.shelf.selected)];
    return info.format == reader::BookFormat::kEbook &&
           reader::EbookDocument::PeekPageImages(info.path.c_str());
}

// 按偏好刷新正文 body 尺寸/边距/底栏（须在 LVGL 任务） 仅 kImmersive 更新 st.viewport_*（供 SetViewport/重开）；浮层保留上次沉浸值
void ApplyReaderPageGeometry(ReaderGeomMode mode) {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.content == nullptr || !lv_obj_is_valid(st.reader.content)) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    const bool show_footer =
        (mode == ReaderGeomMode::kOverlayList) || (BookReaderPrefsHideProgress() == 0);
    const lv_coord_t body_h = ResizeReaderContainers(show_footer);

    // 浮层列表：只调 body/底栏；pad 由 ApplyOverlayListContentPads 设置，且禁止改 viewport_*
    if (mode == ReaderGeomMode::kOverlayList) {
        return;
    }

    const auto area = ReadContentArea();
    // 分页、占位和解码共用扣除页边距及底栏后的视口。
    lv_obj_set_style_pad_top(st.reader.content, area.y1, 0);
    lv_obj_set_style_pad_bottom(st.reader.content, body_h - area.y2 - 1, 0);
    lv_obj_set_style_pad_left(st.reader.content, 0, 0);
    lv_obj_set_style_pad_right(st.reader.content, 0, 0);
    st.reader.viewport_w = lv_area_get_width(&area);
    st.reader.viewport_h = lv_area_get_height(&area);

}

void ApplyReaderChromeSize() {
    auto& st = Book_State();
    if (st.reader.status_bar != nullptr && lv_obj_is_valid(st.reader.status_bar)) {
        lv_obj_set_width(st.reader.status_bar, LV_HOR_RES);
    }
    ResizeReaderContainers(st.reader.read_chrome == BookUiState::ReadChrome::kToc ||
                           BookReaderPrefsHideProgress() == 0);
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    ResizeReadingBottomSheet();
}

void StopReadTimeCheckpointTimer() {
    auto& st = Book_State();
    if (st.reader.read_time_ckpt_timer == nullptr) {
        return;
    }
    lv_timer_del(st.reader.read_time_ckpt_timer);
    st.reader.read_time_ckpt_timer = nullptr;
}

void OnReadTimeCheckpoint(lv_timer_t* /*t*/) {
    auto& st = Book_State();
    // 打开中 / session 已关：跳过；不删 timer（AsyncOpenDone 会重建）
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    const bool ok = st.reader.session->SaveProgress();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings && st.settings.reader_tab == 4 &&
        !ScreenFrontlightIsVisible() && !TouchUiFingerIsDown() && !TouchUiHasPendingEdges()) {
        RequestSettingsSheetPaint();
    }
    const uint32_t sec = st.reader.session->ReadingSeconds();
    const uint32_t daily = st.reader.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGD(TAG, "read-time checkpoint (%us): %s total=%u sec (%u min %u sec) daily=%u sec title=%s",
             static_cast<unsigned>(kReadTimeCheckpointMs / 1000u), ok ? "queued" : "FAIL",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.reader.session->Title().c_str());
}

void StartReadTimeCheckpointTimer() {
    auto& st = Book_State();
    StopReadTimeCheckpointTimer();
    st.reader.read_time_ckpt_timer =
        lv_timer_create(OnReadTimeCheckpoint, kReadTimeCheckpointMs, nullptr);
    if (st.reader.read_time_ckpt_timer == nullptr) {
        ESP_LOGW(TAG, "read time checkpoint timer create failed");
    }
}

// 正文可读后开始累计；须在 LVGL 任务调用
void BeginReadingTimeTracking() {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    const uint32_t sec = st.reader.session->ReadingSeconds();
    const uint32_t daily = st.reader.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time open: total=%u sec (%u min %u sec) daily=%u sec title=%s path=%s",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.reader.session->Title().c_str(),
             st.reader.session->Info().path.c_str());
    st.reader.session->StartReadingClock();
    StartReadTimeCheckpointTimer();
}

// 停止检查点和阅读墙钟；退出快照由 RetireReaderSession 提交，待机由暂停入口提交。
void EndReadingTimeTracking() {
    StopReadTimeCheckpointTimer();
    auto& st = Book_State();
    st.reader.reading_paused_by_standby = false;
    if (!st.reader.opening.load() && st.reader.session && st.reader.session->IsOpen()) {
        st.reader.session->StopReadingClock();
    }
}

// 进待机：停累计；退出待机由 ResumeReadingTimeAfterStandby 恢复
void PauseReadingTimeForStandby() {
    StopReaderAutoTurn();
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsReadingClockActive()) {
        return;
    }
    EndReadingTimeTracking();
    st.reader.session->SaveProgress();
    st.reader.reading_paused_by_standby = true;
    ESP_LOGI(TAG, "read-time paused for standby");
}

void ResumeReadingTimeAfterStandby() {
    auto& st = Book_State();
    if (!st.reader.reading_paused_by_standby) {
        return;
    }
    st.reader.reading_paused_by_standby = false;
    BeginReadingTimeTracking();
    ESP_LOGI(TAG, "read-time resumed after standby");
}

