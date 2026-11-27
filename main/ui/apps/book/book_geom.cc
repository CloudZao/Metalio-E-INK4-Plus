#include <algorithm>
#include <cstdint>
#include <esp_log.h>
#include <lvgl.h>
#include "ui/apps/book/book_geom.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "business/reader/book_tap_zones.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/widgets/screen_common.h"

lv_coord_t ReaderFooterHeight() {
    return static_cast<lv_coord_t>(Book_ItemFont()->line_height + 2 * UiSy(2));
}

static bool SyncReaderSurface() {
    const int width = LV_HOR_RES, height = LV_VER_RES;
    DisplayUiSetFullScreen(BookReaderPrefsBackground() == 3 && !ReaderPageImagesHint());
    if (width == LV_HOR_RES && height == LV_VER_RES) {
        return false;
    }
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestFullOnNextCommit();
    }
    return true;
}

static lv_coord_t ReaderLayoutWidth() {
    return DisplayUiIsLandscape() ? DisplayUiContentH() : DisplayUiContentW();
}

static lv_coord_t ReaderLayoutHeight() {
    return DisplayUiIsLandscape() ? DisplayUiContentW() : DisplayUiContentH();
}

static lv_coord_t ReaderHeaderHeight() {
    const int mask = BookReaderPrefsFooterMask();
    if (BookReaderPrefsFooterLayout() != kBookReaderFooterSplit ||
        (mask & (kBookReaderFooterTitle | kBookReaderFooterProgress)) == 0) {
        return 0;
    }
    const auto* status_label = Book_State().reader.status_label;
    const int text_offset = status_label ? lv_obj_get_style_y(status_label, LV_PART_MAIN) : 0;
    // 顶部文字与状态栏共用垂直位置，容器在文字底边结束。
    return (ScreenStatusBarHeight() + Book_ItemFont()->line_height) / 2 + text_offset;
}

static lv_coord_t ReaderBottomHeight() {
    const int mask = BookReaderPrefsFooterMask();
    if (BookReaderPrefsFooterLayout() == kBookReaderFooterSplit) {
        return (mask & (kBookReaderFooterTime | kBookReaderFooterBattery | kBookReaderFooterProgress)) != 0
            ? ReaderFooterHeight() - UiSy(2) : 0;
    }
    return mask ? ReaderFooterHeight() : 0;
}

static lv_coord_t ResizeReaderContainers(bool show_footer) {
    auto& reader = Book_State().reader;
    const bool toc = reader.read_chrome == BookUiState::ReadChrome::kToc;
    const bool split = !toc && BookReaderPrefsFooterLayout() == kBookReaderFooterSplit;
    const lv_coord_t footer_h = toc ? ReaderFooterHeight() : (show_footer ? ReaderBottomHeight() : 0);
    const lv_coord_t header_h = !toc && show_footer ? ReaderHeaderHeight() : 0;
    // 全屏背景独立覆盖外圈；正文和底栏保持普通 UI 视口及对应的面板位置。
    const bool full = BookReaderPrefsBackground() == 3 && !ReaderPageImagesHint();
    const int left =
        full ? (DisplayUiIsLandscape() ? DISPLAY_CONTENT_TOP_INSET : DISPLAY_CONTENT_LEFT_INSET) + DisplayUiOffsetX()
             : 0;
    const int top =
        full ? (DisplayUiIsLandscape() ? DISPLAY_CONTENT_LEFT_INSET : DISPLAY_CONTENT_TOP_INSET) + DisplayUiOffsetY()
             : 0;
    const lv_coord_t body_h = top + ReaderLayoutHeight() - footer_h;
    if (reader.content && lv_obj_is_valid(reader.content)) {
        lv_obj_set_size(reader.content, LV_HOR_RES, body_h);
        lv_obj_align(reader.content, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_remove_flag(reader.content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    if (reader.footer_host && lv_obj_is_valid(reader.footer_host)) {
        lv_obj_set_size(reader.footer_host, ReaderLayoutWidth(), footer_h);
        lv_obj_align(reader.footer_host, LV_ALIGN_TOP_LEFT, left, body_h);
        lv_obj_set_flag(reader.footer_host, LV_OBJ_FLAG_HIDDEN, footer_h == 0);
    }
    if (reader.page_label && lv_obj_is_valid(reader.page_label)) {
        lv_obj_set_width(reader.page_label, ReaderLayoutWidth() - UiSx(16));
        lv_obj_set_style_text_align(reader.page_label, split ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_flag(reader.page_label, LV_OBJ_FLAG_HIDDEN, !show_footer);
        lv_obj_center(reader.page_label);
    }
    if (reader.info_header) {
        lv_obj_set_size(reader.info_header, ReaderLayoutWidth(), header_h);
        lv_obj_set_pos(reader.info_header, left, top);
        lv_obj_set_flag(reader.info_header, LV_OBJ_FLAG_HIDDEN, header_h == 0);
    }
    if (reader.info_pages) {
        lv_obj_set_flag(reader.info_pages, LV_OBJ_FLAG_HIDDEN, !split);
    }
    return body_h;
}

bool IsReadOverlayChrome(BookUiState::ReadChrome c) {
    using C = BookUiState::ReadChrome;
    return c == C::kToc || c == C::kSettings;
}

// 按偏好刷新正文 body 尺寸/边距/底栏（须在 LVGL 任务） 仅 kImmersive 更新 st.viewport_*（供
// SetViewport/重开）；浮层保留上次沉浸值
void ApplyReaderPageGeometry(ReaderGeomMode mode) {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.content == nullptr || !lv_obj_is_valid(st.reader.content)) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    if (SyncReaderSurface()) {
        ApplyReaderChromeSize();
    }
    const bool show_footer = (mode == ReaderGeomMode::kOverlayList) || (BookReaderPrefsHideProgress() == 0);
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
    SyncReaderSurface();
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
    ESP_LOGD(TAG, "read-time checkpoint (%us): %s total=%u sec (%u min %u sec) daily=%u sec ",
             static_cast<unsigned>(kReadTimeCheckpointMs / 1000u), ok ? "queued" : "FAIL", static_cast<unsigned>(sec),
             static_cast<unsigned>(min), static_cast<unsigned>(rem), static_cast<unsigned>(daily));
}

void StartReadTimeCheckpointTimer() {
    auto& st = Book_State();
    StopReadTimeCheckpointTimer();
    st.reader.read_time_ckpt_timer = lv_timer_create(OnReadTimeCheckpoint, kReadTimeCheckpointMs, nullptr);
    if (st.reader.read_time_ckpt_timer == nullptr) {
        ESP_LOGW(TAG, "read time checkpoint timer create failed");
    }
}

void SyncReaderAaForImmersion(bool text_visible) {
    BookReaderPrefsSyncGlyphAa(text_visible && !ReaderPageImagesHint() ? 1 : 0);
}

void BindSessionLayoutPrefs(BookSession& session) {
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    session.SetFont(BookLayoutFont(), BookFont());
    session.SetTextStyle(BookReaderPrefsTextStyle());
    session.SetVerticalWriting(BookReaderPrefsWritingMode() == kBookReaderWritingVertical && !ReaderPageImagesHint());
    session.SetLineSpacing(BookReaderPrefsLineSpacingPercent(), static_cast<lv_coord_t>(BookReaderPrefsParaGap()));
}

lv_area_t ReadContentArea() {
    BookReaderPrefsEnsureLoaded();
    const bool page_images = ReaderPageImagesHint();
    const bool vertical = !page_images && BookReaderPrefsWritingMode() == kBookReaderWritingVertical;
    const int extra_side = vertical ? kReaderVerticalSideInset : 0;
    const bool full = BookReaderPrefsBackground() == 3 && !page_images;
    const int header_h = ReaderHeaderHeight();
    const int footer_h = ReaderBottomHeight();
    const bool split = BookReaderPrefsFooterLayout() == kBookReaderFooterSplit;
    // 将原信息栏内侧留白与阅读边距合计减半，外侧文字位置不受收紧影响。
    const int top_gap = header_h ? (BookReaderPrefsMarginTop() + UiSy(2)) / 2 : BookReaderPrefsMarginTop();
    const int bottom_gap = split && footer_h ? (BookReaderPrefsMarginBottom() + UiSy(2)) / 2
                                             : BookReaderPrefsMarginBottom();
    const int left =
        (full ? (DisplayUiIsLandscape() ? DISPLAY_CONTENT_TOP_INSET : DISPLAY_CONTENT_LEFT_INSET) + DisplayUiOffsetX()
              : 0) +
        BookReaderPrefsMarginLeft() +
        (vertical && BookReaderPrefsTextDirection() != kBookReaderTextRightToLeft ? extra_side : 0);
    const int top =
        (full ? (DisplayUiIsLandscape() ? DISPLAY_CONTENT_LEFT_INSET : DISPLAY_CONTENT_TOP_INSET) + DisplayUiOffsetY()
              : 0) +
        top_gap + header_h;
    const int width = std::max<int>(
        40, ReaderLayoutWidth() - BookReaderPrefsMarginLeft() - BookReaderPrefsMarginRight() - extra_side);
    const int height =
        std::max<int>(40, ReaderLayoutHeight() - header_h - footer_h - top_gap - bottom_gap);
    return {left, top, left + width - 1, top + height - 1};
}

lv_area_t ReadBackgroundArea() {
    const bool full = BookReaderPrefsBackground() == 3 && !ReaderPageImagesHint();
    const int width = full ? DISPLAY_WIDTH : DisplayUiContentW();
    const int height = full ? DISPLAY_HEIGHT : DisplayUiContentH();
    return DisplayUiIsLandscape() ? lv_area_t{0, 0, height - 1, width - 1} : lv_area_t{0, 0, width - 1, height - 1};
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

// 是否整页图模式（未 Open 时用 Peek，便于建屏设 viewport）
bool ReaderPageImagesHint() {
    auto& st = Book_State();
    if (st.reader.session && st.reader.session->IsOpen()) {
        return st.reader.session->IsPageImagesMode();
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return false;
    }
    const BookInfo& info = st.shelf.books[static_cast<size_t>(st.shelf.selected)];
    return info.format == BookFormat::kEbook && EbookDocument::PeekPageImages(info.path.c_str());
}

void StopReadTimeCheckpointTimer() {
    auto& st = Book_State();
    if (st.reader.read_time_ckpt_timer == nullptr) {
        return;
    }
    lv_timer_del(st.reader.read_time_ckpt_timer);
    st.reader.read_time_ckpt_timer = nullptr;
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
    ESP_LOGI(TAG, "read-time open: total=%u sec (%u min %u sec) daily=%u sec ", static_cast<unsigned>(sec),
             static_cast<unsigned>(min), static_cast<unsigned>(rem), static_cast<unsigned>(daily));
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
