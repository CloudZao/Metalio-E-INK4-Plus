#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/book_vk.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <cstdint>
#include <esp_log.h>
#include <esp_timer.h>

#include "assistant_screen/assistant_screen.h"
#include "book_screen/book_screen.h"
#include "display_orient.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "vk_key_handler.h"

VkKeyScreenDesc BookAiLongPressDesc(ScreenFactory factory) {
    return VkKeyScreenDesc{factory, BookScreen::OnVkKey, BookScreen::OnBootClick,
                           BookScreen::OnBootLongPress, nullptr, nullptr,
                           BookScreen::OnVkKeyLongPress, BookScreen::OnVkKeyPressUp};
}

bool ReadEventPoint(lv_event_t* e, lv_point_t* out) {
    if (out == nullptr) {
        return false;
    }
    lv_indev_t* indev = e != nullptr ? lv_event_get_indev(e) : nullptr;
    if (indev == nullptr) {
        indev = lv_indev_active();
    }
    if (indev == nullptr || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return false;
    }
    lv_indev_get_point(indev, out);
    return true;
}

void DetachReadBodyInput(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_event_cb(obj, OnReadContentClicked);
    // 正文整页热区：进目录/设置后须摘掉，否则空白处仍早震
    HapticDetachClick(obj);
}

void AttachReadBodyInput(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    DetachReadBodyInput(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    // 页内热区：touch_feed 按下早震；业务回调里勿再 Pulse
    HapticAttachClick(obj);
    lv_obj_add_event_cb(obj, OnReadContentClicked, LV_EVENT_CLICKED, nullptr);
}

void ApplyReaderPageDelta() {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen()) {
        s_reader_page_delta.store(0, std::memory_order_release);
        return;
    }
    // 一次吃光积压：长按按档累加，每帧画到「当前指到的页」（墨水慢则中间页跳过）
    int d = s_reader_page_delta.exchange(0, std::memory_order_acq_rel);
    while (d > 0) {
        if (!st.reader.session->NextPage()) {
            break;
        }
        --d;
    }
    while (d < 0) {
        if (!st.reader.session->PrevPage()) {
            break;
        }
        ++d;
    }
}

// 盖板键/长按定时器：只累加 delta，真正翻页在 RenderReaderPage（LVGL）
bool QueueReaderPageTurn(int dir) {
    auto& st = Book_State();
    if (dir == 0 || !st.reader.session || !st.reader.session->IsOpen()) {
        return false;
    }
    const int pending = s_reader_page_delta.load(std::memory_order_acquire);
    if (dir > 0) {
        if (pending <= 0 && !st.reader.session->HasNextPage()) {
            return false;
        }
    } else if (pending >= 0 && !st.reader.session->HasPrevPage()) {
        return false;
    }
    s_reader_page_delta.fetch_add(dir, std::memory_order_acq_rel);
    // 上屏经 ScreenPaintCoalesce：有 pending 则等交完，连点跳到最终页再画
    RequestRenderReaderPage();
    return true;
}

void GoHomeFromBookAsync(void* /*user_data*/) {
    // 勿在此 Reset detail_cover：旧详情屏可能仍引用 dsc，等 DELETE 回调再清
    CancelDetailCoverLoad();
    ReleaseReaderSessionForLeave();
    ScreenRequestBack();
}

void RequestBackHome() {
    BookLvAsync(GoHomeFromBookAsync);
}

void FinishDeferredCleanup() {
    auto& st = Book_State();
    if (!st.reader.deferred_cleanup) {
        return;
    }
    // worker 仍可能握着 session/font_ 计宽：busy 未清不得释字体
    if (st.reader.opening.load() || st.reader.layout_busy.load() || st.reader.chapter_pages_busy.load() ||
        st.reader.page_image_busy.load()) {
        return;
    }
    st.reader.deferred_cleanup = false;
    EndReadingTimeTracking();
    st.reader.session.reset();
    ReleaseBookFont();
}

bool ReaderWorkersBusy() {
    auto& st = Book_State();
    return st.reader.opening.load() || st.reader.layout_busy.load() || st.reader.chapter_pages_busy.load() ||
           st.reader.page_image_busy.load();
}

// 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」
void WarnReaderWorkersBusy(const char* where) {
    auto& st = Book_State();
    if (!ReaderWorkersBusy()) {
        return;
    }
    ESP_LOGW(TAG,
             "%s ignored: open=%d layout=%d chapter_pages=%d page_img=%d deferred_cleanup=%d "
             "open_token=%u sel=%d",
             where, st.reader.opening.load() ? 1 : 0, st.reader.layout_busy.load() ? 1 : 0,
             st.reader.chapter_pages_busy.load() ? 1 : 0, st.reader.page_image_busy.load() ? 1 : 0,
             st.reader.deferred_cleanup ? 1 : 0, static_cast<unsigned>(st.reader.open_token.load()),
             st.shelf.selected);
}

// 离开正文（进百问等）：落盘 .pos、释放 session/字库；打开中则延后清。
void ReleaseReaderSessionForLeave() {
    auto& st = Book_State();
    st.reader.open_token.fetch_add(1);
    st.reader.read_scr = nullptr;
    st.reader.content = nullptr;
    st.reader.footer_host = nullptr;
    st.reader.page_label = nullptr;
    st.reader.title_label = nullptr;
    st.reader.open_bar = nullptr;
    st.reader.open_pct_lbl = nullptr;
    st.reader.status_bar = nullptr;
    st.reader.status_overlay = nullptr;
    st.reader.status_label = nullptr;
    st.reader.status_h = 0;
    st.settings.settings_sheet = nullptr;
    ClearSettingsSheetWidgetRefs();
    StopTtfPollTimer();
    st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
    st.ttf.ttf_pick.clear();
    st.ttf.ttf_page = 0;
    st.ttf.ttf_size_px = kTtfConvertSizeDefault;
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kClosed;
    st.tap.tap_zone_pick_cell = -1;
    st.reader.viewport_w = 0;
    st.reader.viewport_h = 0;
    st.settings.font_entries.clear();
    st.settings.font_list_page = 0;
    st.settings.font_multi = false;
    st.settings.font_selected.clear();
    st.settings.font_suppress_click_until_us = 0;
    st.settings.font_suppress_click_idx = -1;
    st.settings.font_suppress_next_click = false;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    st.reader.settings_resume_after_open = false;
    st.reader.toc_list_page = 0;
    const bool layout_running = st.reader.layout_busy.load();
    const bool chapter_pages_running = st.reader.chapter_pages_busy.load();
    const bool page_image_running = st.reader.page_image_busy.load();
    st.reader.layout_token.fetch_add(1);
    st.reader.layout_again = false;
    st.reader.layout_reload_font = false;
    st.reader.layout_anchor = {};
    st.reader.chapter_pages_token.fetch_add(1);
    st.reader.chapter_pages_again = false;
    CancelPageImageLoad();
    if (st.reader.session) {
        st.reader.session->InvalidateChapterPageTable();
        st.reader.session->AbortTxtPaginate();
    }
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    StopFooterClockTimer();
    st.reader.layout_hint_done_until_us = 0;
    ScreenPaintCoalesceReset(&s_reader_paint);
    ScreenPaintCoalesceReset(&s_toc_paint);
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
    s_reader_page_delta.store(0, std::memory_order_release);
    if (st.reader.opening.load() || layout_running || chapter_pages_running || page_image_running) {
        // 勿把 *_busy 置 false，否则其它路径会误 ReleaseBookFont 与计页抢字体
        StopReadTimeCheckpointTimer();
        st.reader.deferred_cleanup = true;
    } else {
        EndReadingTimeTracking();
        st.reader.session.reset();
        ReleaseBookFont();
    }
    SyncReaderAaForImmersion(false);
    if (DisplayUiSetOrient(kDisplayUiPortrait)) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestNextFullRefresh();
        }
    }
}

lv_obj_t* ResumeReaderScreen() {
    auto& st = Book_State();
    // 解析尚未结束 / 后台计页未结束：退回书库，避免与 worker 抢 session/font
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("ResumeReaderScreen");
        return BookScreen::Create();
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
    } else if (st.reader.session) {
        EndReadingTimeTracking();
        st.reader.session.reset();
        ReleaseBookFont();
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return BookScreen::Create();
    }
    return CreateReaderScreen(st.shelf.books[static_cast<size_t>(st.shelf.selected)]);
}

lv_obj_t* ResumeDetailScreen() {
    auto& st = Book_State();
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return BookScreen::Create();
    }
    return CreateDetailScreen(st.shelf.selected);
}

void OpenAssistantFromReaderAsync(void* /*user_data*/) {
    ReleaseReaderSessionForLeave();
    if (AssistantScreen::IsActive()) {
        ESP_LOGI(TAG, "assistant from reader: already active");
        return;
    }
    HapticPulseIfEnabled();
    ESP_LOGI(TAG, "assistant from reader -> ScreenNavigateTo (resume via .pos)");
    ScreenNavigateTo(AssistantScreen::Create);
}

void RequestOpenAssistantFromReader() {
    auto* d = LVAdapterDisplay::Instance();
    if (d == nullptr || !d->IsReady()) {
        return;
    }
    if (!d->LockUi(-1)) {
        ESP_LOGW(TAG, "assistant from reader: adapter lock failed");
        return;
    }
    if (lv_async_call(OpenAssistantFromReaderAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "assistant from reader: lv_async_call failed");
    }
    d->UnlockUi();
}

// 盖板键在 touch_feed：须 ScreenLvAsync，禁止同步 lv_obj_clean/create（否则 tlsf 双释放）。
bool BookLvAsync(void (*cb)(void*), void* user_data) {
    const int64_t t0 = esp_timer_get_time();
    const bool ok = ScreenLvAsync(cb, user_data);
    ESP_LOGI(TAG, "BookLvAsync queue %s in %d us", ok ? "ok" : "fail",
             static_cast<int>(esp_timer_get_time() - t0));
    return ok;
}

void PaintBookshelfPage() {
    ESP_LOGI(TAG, "async RenderBookshelfPage page=%d begin", Book_State().shelf.list_page);
    RenderBookshelfPage();
    ESP_LOGI(TAG, "async RenderBookshelfPage done");
}

void PaintReaderPage() {
    RenderReaderPage();
}

void PaintTocList() {
    ESP_LOGI(TAG, "async RenderTocList page=%d begin", Book_State().reader.toc_list_page);
    RenderTocList();
    ESP_LOGI(TAG, "async RenderTocList done");
}

void EnsureBookPaintFns() {
    if (s_library_paint.paint == nullptr) {
        s_library_paint.paint = PaintBookshelfPage;
    }
    if (s_reader_paint.paint == nullptr) {
        s_reader_paint.paint = PaintReaderPage;
    }
    if (s_toc_paint.paint == nullptr) {
        s_toc_paint.paint = PaintTocList;
    }
    if (s_settings_sheet_paint.paint == nullptr) {
        s_settings_sheet_paint.paint = RefreshSettingsSheet;
    }
}

// 页码已同步改完后调用：序号合并，积压回调跳过已画过的序号。
void RequestRenderBookshelfPage() {
    EnsureBookPaintFns();
    ScreenPaintCoalesceRequest(&s_library_paint);
}

void RequestRenderReaderPage() {
    EnsureBookPaintFns();
    ScreenPaintCoalesceRequest(&s_reader_paint);
}

void RequestRenderTocList() {
    EnsureBookPaintFns();
    ScreenPaintCoalesceRequest(&s_toc_paint);
}

// 设置卡状态已改完：与正文翻页同形，按下中只记序号，抬起后一次刷最终态
void RequestSettingsSheetPaint() {
    EnsureBookPaintFns();
    ScreenPaintCoalesceRequest(&s_settings_sheet_paint);
}

void AsyncHideReadChrome(void* /*user_data*/) {
    ESP_LOGI(TAG, "async HideReadChrome begin");
    HideReadChrome();
    ESP_LOGI(TAG, "async HideReadChrome done");
}

// BOOT 短按：正文弹出/收起悬浮排版卡片（须 LVGL 任务，禁止 touch_feed 同步改树）。
void ToggleReadSettingsSheetAsync(void* /*user_data*/) {
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        ESP_LOGI(TAG, "async ToggleReadSettingsSheet -> hide");
        HideReadChrome();
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kReading) {
        ESP_LOGI(TAG, "async ToggleReadSettingsSheet -> show");
        ShowReadSettingsSheet();
        return;
    }
    // kToc 目录整页不是悬浮卡片，BOOT 短按不切换
    ESP_LOGI(TAG, "async ToggleReadSettingsSheet -> no-op (toc)");
}

// 盖板 vk_prev：选动作页退回九宫格（须 LVGL 任务，勿在 touch_feed 同步 clean）。
void AsyncTapZonePickBack(void* /*user_data*/) {
    auto& st = Book_State();
    if (st.tap.tap_zone_ui != BookUiState::TapZoneUi::kPick) {
        return;
    }
    st.tap.tap_zone_ui = BookUiState::TapZoneUi::kGrid;
    st.tap.tap_zone_pick_cell = -1;
    TapZonesRebuildContent();
}

// 盖板 vk_prev：关闭触摸分区浮层。
void AsyncTapZonesPanelClose(void* /*user_data*/) {
    TapZonesPanelClose();
}

void ClearDetailCoverUiPtrs() {
    auto& st = Book_State();
    st.detail.detail_cover_host = nullptr;
    st.detail.detail_title_lbl = nullptr;
    st.detail.detail_meta_lbl = nullptr;
}

// 作废进行中的文件内封面补全；须在 LVGL 任务调用。
void CancelDetailCoverLoad() {
    auto& st = Book_State();
    st.detail.detail_cover_token.fetch_add(1);
    ClearDetailCoverUiPtrs();
}

// 作废列表/目录封面补全；须在清槽/离页前调用。
void CancelListCoverFill() {
    auto& st = Book_State();
    st.shelf.list_cover_token.fetch_add(1);
    st.shelf.list_cover_pending.clear();
}

// 作废正文插图后台解码；翻页 clean 前调用。
void CancelPageImageLoad() {
    auto& st = Book_State();
    st.reader.page_image_abort.store(true, std::memory_order_relaxed);
    st.reader.page_image_token.fetch_add(1);
    st.reader.page_image_pending.clear();
    st.reader.page_image_again = false;
}

