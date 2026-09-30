#include "book_screen/book_geom.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <lvgl.h>
#include <cstdint>
#include <esp_log.h>

#include "lv_adapter_display.h"
#include "reader/reader.h"

bool IsReadOverlayChrome(BookUiState::ReadChrome c) {
    using C = BookUiState::ReadChrome;
    return c == C::kToc || c == C::kSettings;
}

void SyncReaderAaForImmersion(bool immersive) {
    if (BookReaderPrefsSyncGlyphAa(immersive ? 1 : 0) == 0) {
        return;
    }
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestDualPlanePartial();
    }
}

void BindSessionLayoutPrefs(reader::BookSession& session) {
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    session.SetFont(BookFont());
    session.SetGaps(static_cast<lv_coord_t>(BookReaderPrefsLineGap()),
                    static_cast<lv_coord_t>(BookReaderPrefsParaGap()));
}

// 正文内容区宽度（阅读页边距）；与沉浸态居中栏宽一致。 书库/详情继续用 Book_ContentWidth()（kListPad），互不影响。
lv_coord_t ReadContentWidth() {
    BookReaderPrefsEnsureLoaded();
    const int ml = BookReaderPrefsMarginLeft();
    const int mr = BookReaderPrefsMarginRight();
    lv_coord_t w = LV_HOR_RES - static_cast<lv_coord_t>(ml + mr);
    return w > 40 ? w : 40;
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
    const lv_coord_t footer_h = show_footer ? kFooterH : 0;
    const lv_coord_t body_h = LV_VER_RES - footer_h;

    if (st.reader.footer_host != nullptr && lv_obj_is_valid(st.reader.footer_host)) {
        if (show_footer) {
            lv_obj_clear_flag(st.reader.footer_host, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.reader.footer_host, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (st.reader.page_label != nullptr && lv_obj_is_valid(st.reader.page_label)) {
        if (show_footer) {
            lv_obj_clear_flag(st.reader.page_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.reader.page_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_set_size(st.reader.content, LV_HOR_RES, body_h);
    lv_obj_align(st.reader.content, LV_ALIGN_TOP_MID, 0, 0);
    // 子控件画出 body 时勿盖住底栏
    lv_obj_remove_flag(st.reader.content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // 浮层列表：只调 body/底栏；pad 由 ApplyOverlayListContentPads 设置，且禁止改 viewport_*
    if (mode == ReaderGeomMode::kOverlayList) {
        return;
    }

    const bool page_images = ReaderPageImagesHint();
    const int mt = BookReaderPrefsMarginTop();
    const int mb = BookReaderPrefsMarginBottom();
    if (page_images) {
        lv_obj_set_style_pad_top(st.reader.content, 0, 0);
        lv_obj_set_style_pad_bottom(st.reader.content, 0, 0);
        lv_obj_set_style_pad_left(st.reader.content, 0, 0);
        lv_obj_set_style_pad_right(st.reader.content, 0, 0);
        st.reader.viewport_w = LV_HOR_RES;
        st.reader.viewport_h = body_h;
    } else {
        // 左右 pad=0：水平居中由 RenderReaderPage 显式算 x，避免 align/flex 失效时贴左
        lv_obj_set_style_pad_top(st.reader.content, static_cast<lv_coord_t>(mt), 0);
        lv_obj_set_style_pad_bottom(st.reader.content, static_cast<lv_coord_t>(mb), 0);
        lv_obj_set_style_pad_left(st.reader.content, 0, 0);
        lv_obj_set_style_pad_right(st.reader.content, 0, 0);
        st.reader.viewport_w = ReadContentWidth();
        lv_coord_t vh =
            body_h - static_cast<lv_coord_t>(mt + mb) - kReadViewportBottomSlack;
        st.reader.viewport_h = vh > 40 ? vh : 40;
    }
}

void ApplyReaderChromeSize() {
    auto& st = Book_State();
    if (st.reader.status_bar != nullptr && lv_obj_is_valid(st.reader.status_bar)) {
        lv_obj_set_width(st.reader.status_bar, LV_HOR_RES);
    }
    if (st.reader.footer_host != nullptr && lv_obj_is_valid(st.reader.footer_host)) {
        lv_obj_set_size(st.reader.footer_host, LV_HOR_RES, kFooterH);
        lv_obj_align(st.reader.footer_host, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_move_foreground(st.reader.footer_host);
    }
    if (st.reader.page_label != nullptr && lv_obj_is_valid(st.reader.page_label)) {
        lv_obj_set_width(st.reader.page_label, LV_HOR_RES - UiSx(16));
        lv_obj_align(st.reader.page_label, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CONTENT_BOTTOM_INSET);
    }
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    lv_obj_t* sheet = st.settings.settings_sheet;
    lv_obj_set_width(sheet, LV_HOR_RES - kSheetSidePad * 2);
    const lv_coord_t top = (st.reader.status_h > 0 ? st.reader.status_h : 40) + 10;
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, top);
    Book_DisableScroll(sheet);
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
    const uint32_t sec = st.reader.session->ReadingSeconds();
    const uint32_t daily = st.reader.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time checkpoint (%us): %s total=%u sec (%u min %u sec) daily=%u sec title=%s",
             static_cast<unsigned>(kReadTimeCheckpointMs / 1000u), ok ? "saved" : "FAIL",
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

// 离开正文前停 checkpoint + 停墙钟（秒数进内存）。 session.reset()/Close 会再 SaveProgress 落盘；本函数不写盘，避免双写路径分叉。
void EndReadingTimeTracking() {
    StopReadTimeCheckpointTimer();
    auto& st = Book_State();
    st.reader.reading_paused_by_standby = false;
    if (st.reader.session && st.reader.session->IsOpen()) {
        st.reader.session->StopReadingClock();
    }
}

// 进待机：停累计；退出待机由 ResumeReadingTimeAfterStandby 恢复
void PauseReadingTimeForStandby() {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsReadingClockActive()) {
        return;
    }
    EndReadingTimeTracking();
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

