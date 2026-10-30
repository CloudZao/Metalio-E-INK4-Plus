#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include <new>
#include "sd_access.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_page_work.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "epd_refresh.h"
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
#include "book_screen/settings/book_font_multi.h"

#include <lvgl.h>
#include <atomic>
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

namespace {
using Kind = reader::BookSession::NavigationKind;
constexpr const char* kTag = "ReaderWork";
std::unique_ptr<ReaderPageResult> ready;
std::unique_ptr<reader::BookSession> retired;
bool pending = false;
Kind pending_kind = Kind::kRefresh;
int pending_value = 0;
int after_absolute_delta = 0;
uint32_t generation = 0;
int64_t requested_us = 0;
int64_t idle_after_us = 0;
bool page_work_busy = false; // 仅由 UI 调度任务读写
int pending_direction = 0; // 合并导航后保留到实际正文发布的移动方向

int ReaderNavigationDirection(const reader::BookSession::LayoutAnchor& from,
                              const reader::BookSession::LayoutAnchor& to) {
    if (!from.valid || !to.valid) return 0;
    if (to.txt) return (to.txt_byte_off > from.txt_byte_off) - (to.txt_byte_off < from.txt_byte_off);
    if (to.chapter != from.chapter) return to.chapter > from.chapter ? 1 : -1;
    return (to.page > from.page) - (to.page < from.page);
}
}

bool RequestReaderNavigation(Kind kind, int value) {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen() || st.reader.opening.load() ||
        st.reader.deferred_cleanup || st.reader.standby_paused) return false;
    if (kind == Kind::kRefresh && pending) return true;
    const bool relative = kind == Kind::kDelta || kind == Kind::kChapterDelta;
    if (kind == Kind::kDelta && pending && pending_kind != kind) {
        after_absolute_delta = std::clamp(after_absolute_delta + std::clamp(value, -128, 128), -128, 128);
    } else if (relative && pending && pending_kind == kind) {
        pending_value = std::clamp(pending_value + std::clamp(value, -128, 128), -128, 128);
    } else {
        pending_kind = kind;
        pending_value = relative ? std::clamp(value, -128, 128) : value;
    }
    if (!relative && kind != Kind::kRefresh) { ++generation; after_absolute_delta = 0; }
    pending = true;
    requested_us = esp_timer_get_time();
    if (kind != Kind::kRefresh || idle_after_us == 0 || st.reader.layout_busy.load() || st.reader.chapter_pages_busy.load()) idle_after_us = requested_us + 1500000;
    if (st.reader.layout_busy.load()) st.reader.session->PauseTxtPaginate();
    if (st.reader.chapter_pages_busy.load() ||
        (page_work_busy && !relative && kind != Kind::kRefresh)) st.reader.session->AbortChapterPageTable();
    CancelPageImageLoad();
    return true;
}

bool ReaderPageWorkPending() {
    return pending || page_work_busy;
}

void CancelReaderPageWork() {
    ++generation;
    pending = false;
    after_absolute_delta = 0;
    pending_direction = 0;
    ready.reset();
    idle_after_us = 0;
    if (Book_State().reader.session) Book_State().reader.session->ResumeTxtPaginate();
}

std::unique_ptr<ReaderPageResult> TakeReaderPageResult() { return std::move(ready); }

void PollReaderPageWork() {
    PollReaderLayoutWork();
    auto& st = Book_State();
    if (st.reader.session && !ReaderPageWorkPending() && !ready &&
        esp_timer_get_time() >= idle_after_us)
        st.reader.session->ResumeTxtPaginate();
    if (st.reader.session && st.reader.session->IsOpen() && !st.reader.opening.load() &&
        !st.reader.deferred_cleanup && !st.reader.standby_paused &&
        !st.reader.layout_again && !st.reader.prefs_reopen_pending) {
        static int64_t progress_after_us = 0;
        const auto now = esp_timer_get_time();
        if (now >= progress_after_us) {
            progress_after_us = now + 100000;
            if (st.reader.session->PollLayoutProgress()) {
                if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) RequestRenderTocList();
                else if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) RequestSettingsSheetPaint();
            }
        }
    }
    if (auto output = TakeCompletedReaderPageWork()) {
        page_work_busy = false;
        ready = std::move(output);
        auto* result = ready.get();
        retired = std::move(result->cursor);
        const bool applicable = result->token == generation && !st.reader.deferred_cleanup &&
            !st.reader.standby_paused && st.reader.content && st.reader.session;
        const auto old_position = applicable ? st.reader.session->CaptureLayoutAnchor() :
            reader::BookSession::LayoutAnchor{};
        if (!applicable) {
            ready.reset();
        } else if (result->ok && st.reader.session->CommitNavigation(*retired)) {
            const auto position = st.reader.session->CaptureLayoutAnchor();
            const int moved = ReaderNavigationDirection(old_position, position);
            if (moved) {
                ReaderAutoTurnPageMoved();
                pending_direction = moved;
            }
            st.reader.session->SaveProgress();
            if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) RequestSettingsSheetPaint();
            if (pending) ready.reset();
            else {
                result->direction = pending_direction;
                pending_direction = 0;
                RequestRenderReaderPage();
            }
        } else {
            if (retired->Error() != reader::BookReadError::kCancelled) {
                static int64_t last_log = 0;
                static unsigned suppressed = 0;
                const auto now = esp_timer_get_time();
                if (last_log == 0 || now - last_log >= 5000000) {
                    ESP_LOGW(kTag, "navigation failed kind=%d phase=%s error=%d chapter=%d page=%d suppressed=%u",
                        static_cast<int>(result->kind), result->ok ? "commit" : "prepare",
                        static_cast<int>(retired->Error()), retired->ChapterIndex(), retired->CurrentPage(), suppressed);
                    last_log = now; suppressed = 0;
                } else ++suppressed;
            }
            if (st.reader.page_label) lv_label_set_text(st.reader.page_label,
                result->kind == Kind::kBookmark ? Lang::Strings::BOOK_BOOKMARK_LOCATE_FAILED : Lang::Strings::BOOK_PAGE_LOAD_FAILED);
            ready.reset();
            if (!pending && pending_direction != 0) {
                // 合并导航后的目标失败时，补画此前已成功提交的位置，仅重试一次。
                pending_direction = 0;
                RequestRenderReaderPage();
            }
        }
        const auto now = esp_timer_get_time();
        if (ready && now - ready->started_us > 500000) {
            static int64_t last_log = 0;
            static unsigned suppressed = 0;
            if (last_log == 0 || now - last_log >= 5000000) {
                ESP_LOGW(kTag, "page preparation kind=%d queue=%lldms navigation=%lldms raster=%lldms total=%lldms suppressed=%u",
                    static_cast<int>(ready->kind), static_cast<long long>(ready->queue_us / 1000),
                    static_cast<long long>(ready->navigation_us / 1000), static_cast<long long>(ready->raster_us / 1000),
                    static_cast<long long>((now - ready->started_us) / 1000), suppressed);
                last_log = now; suppressed = 0;
            } else ++suppressed;
        }
    }
    if (retired) {
        auto* cursor = retired.get();
        if (!ScheduleReaderSessionCleanup(cursor)) return;
        retired.release();
    }
    if (st.reader.deferred_cleanup && !ReaderWorkersBusy()) FinishDeferredCleanup();
    if (st.reader.layout_again && !ReaderWorkersBusy() && st.reader.content && !st.reader.standby_paused) {
        const bool reload = st.reader.layout_reload_font;
        st.reader.layout_again = st.reader.layout_reload_font = false;
        ApplyReaderLayoutLive(reload);
    }
    if (!ReaderPageWorkPending() && !ready && st.reader.content && st.reader.session &&
        st.reader.session->IsOpen() && !st.reader.standby_paused && !st.reader.prefs_reopen_pending && !ReaderWorkersBusy() &&
        esp_timer_get_time() >= idle_after_us) {
        if (st.reader.session->LiveRelayoutPending()) {
            st.reader.layout_anchor = st.reader.session->CaptureLayoutAnchor();
            StartLayoutWorker();
        } else StartChapterPageWorker();
        idle_after_us = INT64_MAX;
    }
    if (!pending || !st.reader.session || !st.reader.content || st.reader.standby_paused || st.reader.deferred_cleanup) return;
    if (st.reader.opening.load() || st.reader.chapter_pages_busy.load() ||
        st.reader.page_image_busy.load() || page_work_busy || st.reader.layout_again || st.reader.prefs_reopen_pending) return;
    if (st.reader.layout_busy.load() && !st.reader.session->PauseTxtPaginate()) return;
    const int followup_delta = after_absolute_delta;
    after_absolute_delta = 0;
    auto result = std::unique_ptr<ReaderPageResult>(new(std::nothrow) ReaderPageResult{});
    if (!result) { ESP_LOGE(kTag, "page result allocation failed"); pending = false; return; }
    int value = pending_value;
    result->cursor = st.reader.session->CreateNavigationCursor(pending_kind, value);
    if (!result->cursor) { pending = false; ESP_LOGE(kTag, "navigation snapshot failed"); return; }
    result->kind = pending_kind; result->value = value;
    result->token = generation; result->started_us = requested_us;
    page_work_busy = true;
    if (!StartReaderPageWork(std::move(result))) {
        page_work_busy = false;
        pending = false;
        return;
    }
    pending = followup_delta != 0;
    pending_kind = Kind::kDelta;
    pending_value = followup_delta;
}

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

bool QueueReaderPageTurn(int dir) {
    if (dir != 0) ReaderAutoTurnManualInput();
    auto& st = Book_State();
    if (dir == 0 || !st.reader.session || !st.reader.session->IsOpen()) return false;
    if (!ReaderPageWorkPending() && (dir > 0 ? !st.reader.session->HasNextPage() : !st.reader.session->HasPrevPage())) return false;
    return RequestReaderNavigation(reader::BookSession::NavigationKind::kDelta, dir);
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
    if (ReaderWorkersBusy()) return;
    if (!RetireReaderSession()) return;
    st.reader.deferred_cleanup = false;
    ReleaseBookFont();
}

bool ScheduleReaderSessionCleanup(reader::BookSession* session) {
    if (!session) return true;
    try {
        sd_access::Permit permit;
        auto access = std::make_shared<sd_access::DeferredTransaction>();
        if (!*access) return false;
        return Application::GetInstance().Schedule([session, access]() {
            for (;;) {
                sd_access::Transaction transaction(*access);
                if (transaction) { delete session; return; }
                vTaskDelay(1);
            }
        }, Application::TaskDomain::Cleanup);
    } catch (const std::bad_alloc&) { return false; }
}

bool RetireReaderSession() {
    auto& st = Book_State();
    EndReadingTimeTracking();
    if (!st.reader.session) return true;
    if (!st.reader.session->PrepareClose()) return false;
    auto* session = st.reader.session.get();
    if (!ScheduleReaderSessionCleanup(session)) return false;
    st.reader.session.release();
    return true;
}

bool ReaderWorkersBusy() {
    auto& st = Book_State();
    return st.reader.opening.load() || st.reader.layout_busy.load() || st.reader.chapter_pages_busy.load() ||
           st.reader.page_image_busy.load() || page_work_busy;
}

// 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」
void WarnReaderWorkersBusy(const char* where) {
    auto& st = Book_State();
    if (!ReaderWorkersBusy()) {
        return;
    }
    ESP_LOGW(TAG,
             "%s ignored: open=%d layout=%d chapter_pages=%d page_img=%d page_work=%d deferred_cleanup=%d "
             "open_token=%u sel=%d",
             where, st.reader.opening.load() ? 1 : 0, st.reader.layout_busy.load() ? 1 : 0,
             st.reader.chapter_pages_busy.load() ? 1 : 0, st.reader.page_image_busy.load() ? 1 : 0,
             page_work_busy ? 1 : 0, st.reader.deferred_cleanup ? 1 : 0, static_cast<unsigned>(st.reader.open_token.load()),
             st.shelf.selected);
}

// 离开正文（进百问等）：落盘 .pos、释放 session/字库；打开中则延后清。
void ReleaseReaderForSystemHome() {
    if (Book_State().reader.read_scr != nullptr) {
        ReleaseReaderSessionForLeave();
    }
}

void ReleaseReaderSessionForLeave() {
    CancelReaderPageWork();
    EndReadingTimeTracking();
    StopReaderAutoOrient();
    CancelReadFontScan();
    CloseBookmarkMessage();
    auto& st = Book_State();
    CancelReaderPrefsReopen();
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
    st.reader.layout_token.fetch_add(1);
    st.reader.layout_again = false;
    st.reader.layout_reload_font = false;
    st.reader.layout_anchor = {};
    st.reader.chapter_pages_token.fetch_add(1);
    CancelPageImageLoad();
    if (st.reader.session) {
        st.reader.session->AbortChapterPageTable();
        st.reader.session->AbortTxtPaginate();
    }
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    StopFooterClockTimer();
    st.reader.layout_hint_done_until_us = 0;
    ScreenPaintCoalesceReset(&s_reader_paint);
    ScreenPaintCoalesceReset(&s_toc_paint);
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
    st.reader.deferred_cleanup = true;
    FinishDeferredCleanup();
    SyncReaderAaForImmersion(false);
    if (DisplayUiSetOrient(kDisplayUiPortrait)) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestFullOnNextCommit();
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
        if (!RetireReaderSession()) return BookScreen::Create();
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
    return CreateReaderScreen(st.shelf.books[static_cast<size_t>(st.shelf.selected)]);
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
    if (!ScreenLvAsyncUrgent(OpenAssistantFromReaderAsync)) {
        ESP_LOGW(TAG, "assistant navigation queue full");
    }
}

// 回调统一经 UI 邮箱执行；投递失败时上下文仍由调用方持有。
bool BookLvAsync(void (*cb)(void*), void* user_data) {
    const bool ok = ScreenLvAsync(cb, user_data);
    if (!ok) ESP_LOGW(TAG, "book UI queue rejected callback=%p", reinterpret_cast<void*>(cb));
    return ok;
}

static std::atomic<bool> s_bookshelf_screen_change{false};

void PaintBookshelfPage() {
    ESP_LOGD(TAG, "async RenderBookshelfPage page=%d begin", Book_State().shelf.list_page);
    const bool screen_change=s_bookshelf_screen_change.exchange(false);
    RenderBookshelfPage();
    if (screen_change && Book_State().shelf.list_scr==lv_screen_active()) {
        EpdCancelPageTurn();
        EpdRequestScreenChange();
        lv_obj_invalidate(lv_screen_active());
    }
    ESP_LOGD(TAG, "async RenderBookshelfPage done");
}

void PaintReaderPage() {
    RenderReaderPage();
}

void PaintTocList() {
    ESP_LOGD(TAG, "async RenderTocList page=%d begin", Book_State().reader.toc_list_page);
    RenderTocList();
    ESP_LOGD(TAG, "async RenderTocList done");
}

void EnsureBookPaintFns() {
    if (s_library_paint.paint == nullptr) {
        s_library_paint.paint = PaintBookshelfPage;
    }
    if (s_reader_paint.paint == nullptr) {
        s_reader_paint.paint = PaintReaderPage;
        s_reader_paint.defer_while_epd_busy = true;
    }
    if (s_toc_paint.paint == nullptr) {
        s_toc_paint.paint = PaintTocList;
    }
    if (s_settings_sheet_paint.paint == nullptr) {
        s_settings_sheet_paint.paint = RefreshSettingsSheet;
        s_settings_sheet_paint.defer_while_epd_busy = true;
    }
}

// 页码已同步改完后调用：序号合并，积压回调跳过已画过的序号。
void RequestRenderBookshelfPage(bool screen_change) {
    if (screen_change) s_bookshelf_screen_change.store(true);
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
    if (CloseBookmarkMessage()) return;
    if(CancelBookmarkMultiSelection())return;
    ESP_LOGI(TAG, "async HideReadChrome begin");
    HideReadChrome();
    ESP_LOGI(TAG, "async HideReadChrome done");
}

// BOOT 短按：正文弹出/收起悬浮排版卡片（须 LVGL 任务，禁止 touch_feed 同步改树）。
void ToggleReadSettingsSheetAsync(void* /*user_data*/) {
    if (CloseBookmarkMessage()) return;
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        if(CancelBookmarkMultiSelection())return;
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

