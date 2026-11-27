#include <algorithm>
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdint>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "business/conversation/conversation_presentation.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/book/book_nav.h"
#include "infra/diagnostics/firmware_memory.h"
#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include "boards/common/storage/sd_access.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/assistant/assistant_screen.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/book_vk.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_open_worker.h"
#include "ui/apps/book/reader/book_page_work.h"
#include "ui/apps/book/reader/book_page_image.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_orient.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/apps/wallpaper/wallpaper_screen.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

using BookNavKind = BookSession::NavigationKind;
static constexpr const char* kTag = "ReaderWork";
static std::unique_ptr<ReaderPageResult> ready;
static std::unique_ptr<BookSession> retired;
static bool pending = false;
static BookNavKind pending_kind = BookNavKind::kRefresh;
static int pending_value = 0;
static uint32_t generation = 0;
static int64_t requested_us = 0;
static const char* navigation_wait = "none"; // 最近一次阻塞前台请求的工作类型
static ReaderPageStage navigation_wait_stage = ReaderPageStage::kIdle;
static int64_t idle_after_us = 0;
static bool page_work_busy = false;                                  // 仅由 UI 调度任务读写
static uint32_t page_work_generation = 0;                            // 在途页面任务接受时的代次
static int pending_direction = 0;                                    // 成功导航后保留到实际正文发布的移动方向
static std::vector<std::unique_ptr<ReaderPageResult>> prefetched; // 按位置接管的可选页面
static int prefetch_failed = 0; // 当前位置已失败的距离，避免无休止重试
static int prefetch_direction = 1; // 最近一次实际翻页方向
static std::recursive_mutex prefetch_mutex; // 保护回收回调与 UI 缓存访问
static std::atomic<bool> prefetch_reclaim_pending{false};
static bool prefetching = false, discard_prefetch = false;           // 单个在途页面任务的预取状态
static int prefetch_value = 0;                                       // 在途预取距当前页的距离

static lv_obj_t* chapter_loading_hint = nullptr;
static int64_t chapter_loading_since_us = 0;

static void HideChapterLoadingHint() {
    chapter_loading_since_us = 0;
    if (chapter_loading_hint) {
        lv_obj_del(chapter_loading_hint);
        chapter_loading_hint = nullptr;
    }
}

static void UpdateChapterLoadingHint() {
    const auto& reader = Book_State().reader;
    if (!reader.read_scr || reader.standby_paused || reader.deferred_cleanup || prefetching ||
        page_work_generation != generation || !ReaderChapterLoadPending()) {
        HideChapterLoadingHint();
        return;
    }
    const auto now = esp_timer_get_time();
    if (chapter_loading_since_us == 0) {
        chapter_loading_since_us = now;
    }
    if (!chapter_loading_hint && now - chapter_loading_since_us >= 150000) {
        chapter_loading_hint = ScreenCreateToast(reader.read_scr, LangString_BOOK_CHAPTER_LOADING);
        lv_obj_add_event_cb(chapter_loading_hint, [](lv_event_t*) {
            chapter_loading_hint = nullptr;
        }, LV_EVENT_DELETE, nullptr);
    }
}

static UiPageLifecycle s_reader_lifecycle;
static std::atomic<bool> s_reader_update_queued{false};
static ScreenUiCompletion s_reader_update;

static void UpdateReaderRuntime(ScreenUiCompletion*) {
    s_reader_update_queued.store(false, std::memory_order_release);
    BookApplyDeferredOpenResult();
    PollReaderPageWork();
    BookScreen::PollStandbyWork();
    const auto& reader = Book_State().reader;
    if (reader.deferred_cleanup || retired ||
        (reader.standby_paused && (!reader.standby_detached || !StandbyPresentation::IsActive()))) {
        BookRequestUiWork();
    }
}

void BookRequestUiWork() {
    if (!s_reader_update_queued.exchange(true, std::memory_order_acq_rel)) {
        s_reader_update.apply = UpdateReaderRuntime;
        ScreenCompleteWork(&s_reader_update, true);
    }
}

void BookBindPageLifecycle(lv_obj_t* root, bool reader) {
    UiPageHooks hooks;
    if (reader) {
        hooks.update = []() { PollReaderPageWork(); };
        hooks.update_ms = 50;
    }
    hooks.power = [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications) {
            BookScreen::OnEnterStandby();
            BookRequestUiWork();
        } else if (event == UiPagePowerEvent::ResumeViews) {
            BookScreen::OnResumeFromStandby();
            BookRequestUiWork();
        }
    };
    s_reader_lifecycle.Bind(root, hooks);
}

static bool SameReaderPosition(const BookSession::LayoutAnchor& a, const BookSession::LayoutAnchor& b) {
    if (!a.valid || !b.valid || a.txt != b.txt) {
        return false;
    }
    if (a.txt) {
        return a.txt_byte_off == b.txt_byte_off;
    }
    if (a.source_block != UINT32_MAX && b.source_block != UINT32_MAX) {
        return a.source_chapter == b.source_chapter && a.source_block == b.source_block &&
               a.source_offset == b.source_offset;
    }
    return a.chapter == b.chapter && a.page == b.page;
}

static size_t PrefetchBytes() {
    size_t bytes = 0;
    for (const auto& page : prefetched) {
        if (page) {
            bytes += page->raster.pixels.capacity();
        }
    }
    return bytes;
}

static bool CanPrefetchReaderPage() {
    const auto& st = Book_State();
    const size_t frame = size_t(std::max<int>(0, st.reader.viewport_w)) * std::max<int>(0, st.reader.viewport_h);
    const size_t used = PrefetchBytes();
    return frame && FirmwarePrefetchAllowed() &&
           used + frame <= FirmwarePsramCacheBudget(used) &&
           FirmwarePsramCanAllocate(frame * 2);
}

static void ReclaimReaderPrefetch(void*) {
    prefetch_reclaim_pending.store(true);
    std::unique_lock<std::recursive_mutex> lock(prefetch_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        // 游标可能拥有 SD 工作文件，留在 UI 清理；先归还独占的大块像素。
        for (auto& page : prefetched) {
            std::vector<uint8_t>().swap(page->raster.pixels);
            page->background.reset();
        }
        if (prefetching) {
            CancelReaderPagePreparation();
        }
    }
    BookRequestUiWork();
}

static int ReaderNavigationDirection(const BookSession::LayoutAnchor& from, const BookSession::LayoutAnchor& to) {
    if (!from.valid || !to.valid) {
        return 0;
    }
    if (to.txt) {
        return (to.txt_byte_off > from.txt_byte_off) - (to.txt_byte_off < from.txt_byte_off);
    }
    if (to.source_block != UINT32_MAX && from.source_block != UINT32_MAX) {
        const BookSourcePosition start{from.source_chapter, from.source_block, from.source_offset};
        const BookSourcePosition end{to.source_chapter, to.source_block, to.source_offset};
        return static_cast<int>(SourceBefore(start, end)) - static_cast<int>(SourceBefore(end, start));
    }
    if (to.chapter != from.chapter) {
        return to.chapter > from.chapter ? 1 : -1;
    }
    return (to.page > from.page) - (to.page < from.page);
}

VkKeyScreenDesc BookAiLongPressDesc(ScreenFactory factory) {
    auto desc = VkKeyScreenDesc{
        factory, BookScreen::OnVkKey,          BookScreen::OnBootClick,   BookScreen::OnBootLongPress, nullptr,
        nullptr, BookScreen::OnVkKeyLongPress, BookScreen::OnVkKeyPressUp};
    desc.volume_keys_turn_pages = BookScreen::IsReadingActive;
    desc.on_page_key = BookScreen::OnPageKey;
    return desc;
}

lv_obj_t* ResumeDetailScreen() {
    auto& st = Book_State();
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return BookScreen::Create();
    }
    return CreateReaderScreen(st.shelf.books[static_cast<size_t>(st.shelf.selected)]);
}

static lv_obj_t* ResumeReadingSettings() {
    auto& st = Book_State();
    if (!ReaderWorkersBusy()) {
        FinishDeferredCleanup();
    }
    if (ReaderWorkersBusy() || st.reader.deferred_cleanup) {
        // 快速退出壁纸时，等待旧会话的工作任务收尾，再恢复正文与设置。
        auto* screen = lv_obj_create(nullptr);
        Book_DisableScroll(screen);
        Book_ShowMessage(screen, LangString_BOOK_LOADING, Book_ListFont());
        auto* timer = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* origin = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
                if (lv_screen_active() != origin || ReaderWorkersBusy() || !EpdTryBeginUiRender()) {
                    return;
                }
                struct UiGuard {
                    ~UiGuard() {
                        EpdEndUiRender();
                    }
                } guard;
                FinishDeferredCleanup();
                if (Book_State().reader.deferred_cleanup) {
                    return;
                }
                ScreenLoadReplace(ResumeReadingSettings());
            },
            50, screen);
        if (!timer) {
            lv_obj_delete(screen);
            return BookScreen::Create();
        }
        lv_obj_add_event_cb(
            screen, [](lv_event_t* e) { lv_timer_delete(static_cast<lv_timer_t*>(lv_event_get_user_data(e))); },
            LV_EVENT_DELETE, timer);
        VkKey_AttachScreen(screen, kScreenRead, BookAiLongPressDesc(ResumeReadingSettings));
        return screen;
    }
    auto* screen = ResumeReaderScreen();
    if (screen == st.reader.read_scr) {
        st.reader.settings_resume_after_open = true;
    }
    return screen;
}

void PaintBookshelfPage() {
    ESP_LOGD(TAG, "async RenderBookshelfPage page=%d begin", Book_State().shelf.list_page);
    RenderBookshelfPage();
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

void AsyncHideReadChrome(void* /*user_data*/) {
    if (CloseBookmarkMessage()) {
        return;
    }
    if (CancelBookmarkMultiSelection()) {
        return;
    }
    ESP_LOGI(TAG, "async HideReadChrome begin");
    HideReadChrome();
    ESP_LOGI(TAG, "async HideReadChrome done");
}

// BOOT 短按：正文弹出/收起悬浮排版卡片（须 LVGL 任务，禁止 touch_feed 同步改树）。
void ToggleReadSettingsSheetAsync(void* /*user_data*/) {
    if (CloseBookmarkMessage()) {
        return;
    }
    auto& st = Book_State();
    if (st.reader.read_scr == nullptr || st.reader.opening.load() || !st.reader.session ||
        !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        if (CancelBookmarkMultiSelection()) {
            return;
        }
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

bool QueueReaderPageTurn(int dir) {
    if (dir != 0) {
        ReaderAutoTurnManualInput();
    }
    auto& st = Book_State();
    if (dir == 0 || !st.reader.session || !st.reader.session->IsOpen()) {
        return false;
    }
    return RequestReaderNavigation(BookSession::NavigationKind::kDelta, dir);
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
    if (ReaderWorkersBusy()) {
        return;
    }
    if (!RetireReaderSession()) {
        return;
    }
    st.reader.deferred_cleanup = false;
    ReleaseBookFont();
}

bool RetireReaderSession() {
    auto& st = Book_State();
    EndReadingTimeTracking();
    if (!st.reader.session) {
        return true;
    }
    if (!st.reader.session->PrepareClose()) {
        return false;
    }
    auto* session = st.reader.session.get();
    if (!ScheduleReaderSessionCleanup(session)) {
        return false;
    }
    st.reader.session.release();
    return true;
}

bool ScheduleReaderSessionCleanup(BookSession* session) {
    if (!session) {
        return true;
    }
    try {
        SdAccessPermit permit;
        auto access = std::make_shared<SdAccessDeferredTransaction>();
        if (!*access) {
            return false;
        }
        return TaskExecutor::Schedule(
            [session, access]() {
                for (;;) {
                    SdAccessTransaction transaction(*access);
                    if (transaction) {
                        delete session;
                        return;
                    }
                    vTaskDelay(1);
                }
            },
            TaskDomain::Cleanup);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool ReaderWorkersBusy() {
    auto& st = Book_State();
    return st.reader.opening.load() || st.reader.layout_busy.load() || st.reader.page_image_busy.load() ||
           page_work_busy || BookFontReloadBusy();
}

// 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」
void WarnReaderWorkersBusy(const char* where) {
    auto& st = Book_State();
    if (!ReaderWorkersBusy()) {
        return;
    }
    ESP_LOGW(TAG,
             "%s ignored: open=%d layout=%d page_img=%d page_work=%d deferred_cleanup=%d "
             "open_token=%u sel=%d",
             where, st.reader.opening.load() ? 1 : 0, st.reader.layout_busy.load() ? 1 : 0,
             st.reader.page_image_busy.load() ? 1 : 0,
             page_work_busy ? 1 : 0, st.reader.deferred_cleanup ? 1 : 0,
             static_cast<unsigned>(st.reader.open_token.load()), st.shelf.selected);
}

void ReleaseReaderSessionForLeave() {
    CancelReaderPageWork();
    EndReadingTimeTracking();
    StopReaderAutoOrient();
    CancelReadFontScan();
    CloseBookmarkMessage();
    auto& st = Book_State();
    const bool refresh_reader_aa = st.reader.read_scr != nullptr && BookReaderPrefsAntialias();
    CancelReaderPrefsReopen();
    st.reader.open_token.fetch_add(1);
    st.reader.read_scr = nullptr;
    st.reader.background_image.reset();
    st.reader.content = nullptr;
    st.reader.footer_host = nullptr;
    st.reader.page_label = nullptr;
    st.reader.info_header = nullptr;
    st.reader.info_title = nullptr;
    st.reader.info_progress = nullptr;
    st.reader.info_pages = nullptr;
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
    CancelPageImageLoad();
    if (st.reader.session) {
        st.reader.session->AbortChapterLayout();
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
    BookRequestUiWork();
    SyncReaderAaForImmersion(false);
    DisplayUiSetFullScreen(false);
    if (DisplayUiSetOrient(kDisplayUiPortrait) || refresh_reader_aa) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestFullOnNextCommit();
        }
    }
}

// 导航确定执行后清理当前正文；等待或取消导航时保留活动页面的会话和字库。
void ReleaseReaderForSystemHome() {
    auto* read_scr = Book_State().reader.read_scr;
    if (read_scr != nullptr && read_scr == lv_screen_active()) {
        ReleaseReaderSessionForLeave();
    }
}

lv_obj_t* ResumeReaderScreen() {
    auto& st = Book_State();
    // 解析或当前章节重排尚未结束时返回书库，资源由任务完成后的回收路径释放。
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("ResumeReaderScreen");
        return BookScreen::Create();
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
    } else if (st.reader.session) {
        if (!RetireReaderSession()) {
            return BookScreen::Create();
        }
        ReleaseBookFont();
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        return BookScreen::Create();
    }
    return CreateReaderScreen(st.shelf.books[static_cast<size_t>(st.shelf.selected)]);
}

void RequestOpenAssistantFromReader() {
    if (!ScreenLvAsyncUrgent(OpenAssistantFromReaderAsync)) {
        ESP_LOGW(TAG, "assistant navigation queue full");
    }
}

void OpenReadingWallpaper() {
    ScreenNavigateTo(WallpaperScreen::CreateForReading, ResumeReadingSettings);
}

// 回调统一经 UI 邮箱执行；投递失败时上下文仍由调用方持有。
bool BookLvAsync(void (*cb)(void*), void* user_data) {
    const bool ok = ScreenLvAsync(cb, user_data);
    if (!ok) {
        ESP_LOGW(TAG, "book UI queue rejected callback=%p", reinterpret_cast<void*>(cb));
    }
    return ok;
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
void RequestRenderBookshelfPage(bool screen_change) {
    if (screen_change) {
        Book_State().shelf.screen_change_pending.store(true);
    }
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

void GoHomeFromBookAsync(void* /*user_data*/) {
    // 勿在此 Reset detail_cover：旧详情屏可能仍引用 dsc，等 DELETE 回调再清
    CancelDetailCoverLoad();
    ScreenRequestHome();
}

void OpenAssistantFromReaderAsync(void* /*user_data*/) {
    if (ConversationPresentation::IsActive()) {
        ESP_LOGI(TAG, "assistant from reader: already active");
        return;
    }
    HapticPulseIfEnabled();
    ESP_LOGI(TAG, "assistant from reader -> ScreenNavigateTo (resume via .pos)");
    ScreenNavigateTo(AssistantScreen::Create);
}

void ClearDetailCoverUiPtrs() {
    auto& st = Book_State();
    st.detail.detail_cover_host = nullptr;
    st.detail.detail_title_lbl = nullptr;
    st.detail.detail_meta_lbl = nullptr;
}

// 作废进行中的文件内封面补全；须在 LVGL 任务调用。
void CancelDetailCoverLoad() {
    CancelDetailCoverWorker();
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

bool RequestReaderNavigation(BookNavKind kind, int value) {
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen() || st.reader.opening.load() || st.reader.deferred_cleanup ||
        st.reader.standby_paused) {
        return false;
    }
    const bool relative = kind == BookNavKind::kDelta || kind == BookNavKind::kChapterDelta;
    if (relative && (pending || ready || (page_work_busy && !prefetching) ||
                     (st.reader.layout_busy.load() && !st.reader.session->StructurePending()) ||
                     BookFontReloadBusy() || st.reader.layout_again ||
                     st.reader.prefs_reopen_pending ||
                     s_reader_paint.seq.load() != s_reader_paint.done.load())) {
        // 忙碌时消费输入，不能返回边界失败，否则上一页按键会退出阅读。
        return true;
    }
    if (kind == BookNavKind::kDelta &&
        (value > 0 ? !st.reader.session->HasNextPage() : !st.reader.session->HasPrevPage())) {
        return false;
    }
    if (BookFontReloadBusy()) {
        return false;
    }
    if (kind == BookNavKind::kRefresh &&
        (pending || ready || (page_work_busy && !prefetching && page_work_generation == generation))) {
        return true;
    }
    pending_kind = kind;
    pending_value = relative ? std::clamp(value, -128, 128) : value;
    if (!relative && kind != BookNavKind::kRefresh) {
        ++generation;
    }
    HideChapterLoadingHint();
    pending = true;
    requested_us = esp_timer_get_time();
    navigation_wait = "none";
    navigation_wait_stage = ReaderPageStage::kIdle;
    if (kind != BookNavKind::kRefresh || idle_after_us == 0 || st.reader.layout_busy.load()) {
        idle_after_us = requested_us + 1500000;
    }
    const bool waits_for_structure = kind == BookNavKind::kProgress && value != 0 &&
                                     st.reader.session->StructurePending() && st.reader.session->LiveRelayoutPending() &&
                                     st.reader.session->Info().format != BookFormat::kTxt;
    if ((st.reader.layout_busy.load() && !waits_for_structure) ||
        (page_work_busy && !relative && kind != BookNavKind::kRefresh)) {
        if (relative && st.reader.layout_busy.load()) {
            // 先保留翻页，再作废旧锚点结果；等待任务退出后使用现有预览继续阅读。
            st.reader.layout_token.fetch_add(1);
        }
        st.reader.session->AbortChapterLayout();
    }
    if (prefetching && pending_kind == BookNavKind::kDelta && pending_value == prefetch_value) {
        PromoteReaderPagePreparation();
    }
    if (prefetching && (pending_kind != BookNavKind::kDelta || pending_value != prefetch_value)) {
        discard_prefetch = true;
        CancelReaderPagePreparation();
    }
    CancelPageImageLoad();
    return true;
}

static void PublishReaderPage(std::unique_ptr<ReaderPageResult> output) {
    auto& st = Book_State();
    ready = std::move(output);
    auto* result = ready.get();
    retired = std::move(result->cursor);
    const bool applicable = result->token == generation && !st.reader.deferred_cleanup &&
                            !st.reader.standby_paused && st.reader.content && st.reader.session;
    const auto old_position = applicable ? st.reader.session->CaptureLayoutAnchor() : BookSession::LayoutAnchor{};
    if (!applicable) {
        ready.reset();
    } else if (result->ok && st.reader.session->CommitNavigation(*retired)) {
        const auto position = st.reader.session->CaptureLayoutAnchor();
        const int moved = ReaderNavigationDirection(old_position, position);
        if (moved) {
            ReaderAutoTurnPageMoved();
            pending_direction = moved;
            prefetch_direction = moved;
        }
        // 跨章更换分页文件；同章页面可以继续接管已有游标。
        if (result->kind == BookNavKind::kDelta && moved && old_position.chapter == position.chapter) {
            for (auto& page : prefetched) {
                page->value -= result->value;
            }
            prefetched.erase(std::remove_if(prefetched.begin(), prefetched.end(),
                                            [](const auto& page) { return page->value == 0; }), prefetched.end());
            prefetch_failed = 0;
        } else if (result->kind != BookNavKind::kRefresh) {
            InvalidateReaderPagePrefetch();
        }
        st.reader.session->SaveProgress();
        if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
            RequestSettingsSheetPaint();
        }
        if (pending) {
            ready.reset();
        } else {
            result->direction = pending_direction;
            pending_direction = 0;
            RequestRenderReaderPage();
        }
    } else {
        if (st.reader.session->NeedsCurrentPagePreview()) {
            st.reader.session->ClearLivePreview();
        }
        if (retired->Error() != BookReadError::kCancelled) {
            static int64_t last_log = 0;
            static unsigned suppressed = 0;
            const auto now = esp_timer_get_time();
            if (last_log == 0 || now - last_log >= 5000000) {
                ESP_LOGW(kTag, "navigation failed kind=%d phase=%s error=%d chapter=%d page=%d suppressed=%u",
                         static_cast<int>(result->kind), result->ok ? "commit" : "prepare",
                         static_cast<int>(retired->Error()), retired->ChapterIndex(), retired->CurrentPage(),
                         suppressed);
                last_log = now;
                suppressed = 0;
            } else {
                ++suppressed;
            }
        }
        if (st.reader.page_label) {
            lv_label_set_text(st.reader.page_label, result->kind == BookNavKind::kBookmark
                                                        ? LangString_BOOK_BOOKMARK_LOCATE_FAILED
                                                        : LangString_BOOK_PAGE_LOAD_FAILED);
        }
        ready.reset();
        if (!pending && pending_direction != 0) {
            // 后续导航目标失败时，补画此前已成功提交的位置，仅重试一次。
            pending_direction = 0;
            RequestRenderReaderPage();
        }
    }
    const auto now = esp_timer_get_time();
    if (ready && now - ready->started_us > 500000) {
        static int64_t last_log = 0;
        static unsigned suppressed = 0;
        if (last_log == 0 || now - last_log >= 5000000) {
            ESP_LOGW(
                kTag,
                "page preparation kind=%d queue=%lldms navigation=%lldms raster=%lldms total=%lldms wait=%s stage=%d suppressed=%u",
                static_cast<int>(ready->kind), static_cast<long long>(ready->queue_us / 1000),
                static_cast<long long>(ready->navigation_us / 1000),
                static_cast<long long>(ready->raster_us / 1000),
                static_cast<long long>((now - ready->started_us) / 1000), navigation_wait,
                static_cast<int>(navigation_wait_stage), suppressed);
            last_log = now;
            suppressed = 0;
        } else {
            ++suppressed;
        }
    }
}

void PollReaderPageWork() {
    static FirmwareCacheReclaimer reclaimer(ReclaimReaderPrefetch, nullptr);
    std::lock_guard<std::recursive_mutex> cache_lock(prefetch_mutex);
    if (prefetch_reclaim_pending.exchange(false)) {
        InvalidateReaderPagePrefetch();
    }
    PollReaderLayoutWork();
    UpdateChapterLoadingHint();
    auto& st = Book_State();
    const bool apply_font = st.reader.content && st.reader.session && !st.reader.standby_paused &&
                            !st.reader.deferred_cleanup && !st.reader.layout_again;
    const auto font_result = PollBookFontReload(apply_font);
    if (font_result == BookFontReloadResult::kApplied) {
        ApplyReaderLayoutLive(false);
    } else if (font_result == BookFontReloadResult::kFailed) {
        ScreenShowToast(LangString_TTF_APPLY_FAILED);
    }
    auto output = TakeCompletedReaderPageWork();
    if (output && output->prefetch) {
        page_work_busy = prefetching = false;
        if ((!output->navigation_ready ||
             (!output->chapter_only && (!output->ok || output->raster.empty()))) &&
            !output->deferred && !discard_prefetch) {
            prefetch_failed = prefetch_value;
        }
        if (!discard_prefetch && output->token == generation && output->navigation_ready && st.reader.session &&
            ((output->ok && !output->raster.empty()) ||
             output->cursor->ChapterIndex() != st.reader.session->ChapterIndex()) &&
            output->reclaim_generation == FirmwareMemoryReclaimGeneration() && FirmwarePrefetchAllowed() &&
            !st.reader.deferred_cleanup && !st.reader.standby_paused) {
            if (!output->ok || output->raster.empty()) {
                output->raster.Reset();
                output->background.reset();
                std::vector<int>().swap(output->image_tops);
                output->ok = true;
                output->chapter_only = true;
            }
            const auto target = output->cursor->CaptureLayoutAnchor();
            bool duplicate = SameReaderPosition(target, st.reader.session->CaptureLayoutAnchor());
            for (const auto& page : prefetched) {
                if (page && SameReaderPosition(target, page->cursor->CaptureLayoutAnchor())) {
                    duplicate = true;
                }
            }
            if (!duplicate) {
                try {
                    prefetched.push_back(std::move(output));
                } catch (const std::bad_alloc&) {
                    prefetch_failed = prefetch_value;
                    ESP_LOGD(kTag, "operation=prefetch stage=retain result=no_memory");
                }
            } else {
                prefetch_failed = prefetch_value;
            }
        }
        output.reset();
    }
    if (output) {
        page_work_busy = false;
        PublishReaderPage(std::move(output));
    }
    if (retired) {
        auto* cursor = retired.get();
        if (!ScheduleReaderSessionCleanup(cursor)) {
            return;
        }
        retired.release();
    }
    if (st.reader.deferred_cleanup && !ReaderWorkersBusy()) {
        FinishDeferredCleanup();
    }
    if (st.reader.layout_again && !ReaderWorkersBusy() && st.reader.content && !st.reader.standby_paused) {
        const bool reload = st.reader.layout_reload_font;
        st.reader.layout_again = st.reader.layout_reload_font = false;
        ApplyReaderLayoutLive(reload);
    }
    static uint32_t resumed_generation = 0;
    const uint32_t resumed = FirmwarePrefetchResumeGeneration();
    if (resumed != resumed_generation && FirmwarePrefetchAllowed() && !ReaderWorkersBusy()) {
        resumed_generation = resumed;
        prefetch_failed = 0;
        if (!pending && st.reader.session && st.reader.session->IsPageImagesMode() &&
            st.reader.session->CanPrefetchPageImage()) {
            StartPageImageWorker();
        }
    }
    if (idle_after_us == INT64_MAX && st.reader.session && !st.reader.layout_busy.load() &&
        st.reader.session->LiveRelayoutPending()) {
        idle_after_us = esp_timer_get_time() + 1500000;
    }
    const bool waits_for_structure = pending && pending_kind == BookNavKind::kProgress && pending_value != 0 &&
                                     st.reader.session && st.reader.session->StructurePending() && st.reader.session->LiveRelayoutPending() &&
                                     st.reader.session->Info().format != BookFormat::kTxt;
    if (!ReaderPageWorkPending() && !ready && st.reader.content && st.reader.session && st.reader.session->IsOpen() &&
        !st.reader.standby_paused && !st.reader.deferred_cleanup && !st.reader.layout_again &&
        !st.reader.prefs_reopen_pending && !st.reader.layout_debounce_timer && !st.reader.opening.load() &&
        !st.reader.page_image_busy.load() && !page_work_busy &&
        !BookFontReloadBusy() &&
        (!st.reader.layout_busy.load() || st.reader.session->Info().format == BookFormat::kTxt) &&
        (!st.reader.session->LiveRelayoutPending() || st.reader.session->Info().format == BookFormat::kTxt) &&
        !st.reader.session->IsPageImagesMode() &&
        (prefetch_direction > 0 ? st.reader.session->HasNextPage() : st.reader.session->HasPrevPage()) &&
        s_reader_paint.seq.load() == s_reader_paint.done.load() && FirmwarePrefetchAllowed()) {
        auto& session = *st.reader.session;
        const bool adjacent = prefetch_direction > 0
                                  ? session.ChapterIndex() + 1 < session.ChapterCount()
                                  : session.ChapterIndex() > 0;
        const int boundary = prefetch_direction > 0 ? session.PageCount() - session.CurrentPage()
                                                   : -session.CurrentPage() - 1;
        const bool prepare_chapter = adjacent && !session.HasCurrentPagePreview();
        for (int offset = 1; size_t(offset) <= prefetched.size() + 3; ++offset) {
            const int distance = offset == 2 && prepare_chapter ? boundary
                                 : (offset > 2 && prepare_chapter ? offset - 1 : offset) * prefetch_direction;
            const int target = st.reader.session->CurrentPage() + distance;
            if ((!st.reader.session->HasCurrentPagePreview() &&
                 (target < -1 || target > st.reader.session->PageCount()))) {
                break;
            }
            if (distance == prefetch_failed) {
                continue;
            }
            auto found = std::find_if(prefetched.begin(), prefetched.end(),
                                      [distance](const auto& page) { return page->value == distance; });
            if (found != prefetched.end() && !(*found)->raster.empty()) {
                continue;
            }
            const bool chapter_only = prepare_chapter && distance == boundary && found == prefetched.end();
            const size_t frame = size_t(std::max<int>(0, st.reader.viewport_w)) *
                                 std::max<int>(0, st.reader.viewport_h);
            if (chapter_only ? !FirmwarePsramCanAllocate(frame) : !CanPrefetchReaderPage()) {
                if (offset == 1 && prepare_chapter) {
                    continue;
                }
                break;
            }
            if (st.reader.layout_busy.load()) {
                break;
            }
            auto page = found != prefetched.end() ? std::move(*found)
                : std::unique_ptr<ReaderPageResult>(new (std::nothrow) ReaderPageResult);
            if (found != prefetched.end()) {
                prefetched.erase(found);
            }
            if (!page) {
                prefetch_failed = distance;
                ESP_LOGD(kTag, "operation=prefetch stage=create_result result=no_memory");
                break;
            }
            int value = distance;
            if (!page->navigation_ready) {
                page->cursor = st.reader.session->CreateNavigationCursor(BookNavKind::kDelta, value);
            }
            page->chapter_only = chapter_only;
            if (!page->cursor) {
                prefetch_failed = distance;
                ESP_LOGD(kTag, "operation=prefetch stage=create_cursor result=unavailable");
                break;
            }
            page->kind = BookNavKind::kDelta;
            page->value = distance;
            page->prefetch = true;
            page->token = generation;
            page->started_us = esp_timer_get_time();
            discard_prefetch = false;
            if (StartReaderPageWork(std::move(page))) {
                page_work_generation = generation;
                page_work_busy = prefetching = true;
                prefetch_value = distance;
            } else {
                prefetch_failed = distance;
            }
            break;
        }
    }
    if (!page_work_busy && !ready && st.reader.content && st.reader.session &&
        (!pending || waits_for_structure || st.reader.session->HasCurrentPagePreview()) && st.reader.session->IsOpen() &&
        !st.reader.standby_paused && !st.reader.prefs_reopen_pending && !ReaderWorkersBusy() &&
        s_reader_paint.seq.load() == s_reader_paint.done.load() &&
        (esp_timer_get_time() >= idle_after_us || st.reader.session->HasCurrentPagePreview())) {
        if (st.reader.session->LiveRelayoutPending()) {
            st.reader.layout_anchor = st.reader.session->CaptureLayoutAnchor();
            StartLayoutWorker();
        }
        idle_after_us = INT64_MAX;
    }
    if (!pending || !st.reader.session || !st.reader.content || st.reader.standby_paused ||
        st.reader.deferred_cleanup) {
        return;
    }
    // 全书百分比定位等待结构结果；源锚点翻页不需要完整目录。
    if (waits_for_structure || st.reader.session->HasCurrentPagePreview()) {
        navigation_wait = "structure";
        return;
    }
    if (pending_kind == BookNavKind::kDelta && prefetching && !retired &&
        ReaderPagePreparationStage() != ReaderPageStage::kNavigation &&
        !st.reader.opening.load() && !st.reader.page_image_busy.load() && !BookFontReloadBusy() &&
        !st.reader.layout_busy.load() && !st.reader.layout_again && !st.reader.prefs_reopen_pending) {
        const auto found = std::find_if(prefetched.begin(), prefetched.end(), [](const auto& page) {
            return page->value == pending_value && !page->raster.empty();
        });
        if (found != prefetched.end()) {
            auto page = std::move(*found);
            prefetched.erase(found);
            page->prefetch = false;
            page->started_us = requested_us;
            page->queue_us = esp_timer_get_time() - requested_us;
            page->navigation_us = page->raster_us = 0;
            pending = false;
            pending_value = 0;
            // 在途游标基于旧位置；保留 busy 到交付，禁止其迟到结果重新进入缓存。
            discard_prefetch = true;
            CancelReaderPagePreparation();
            PublishReaderPage(std::move(page));
            return;
        }
    }
    if (st.reader.opening.load() || st.reader.page_image_busy.load() ||
        page_work_busy || BookFontReloadBusy() || st.reader.layout_again || st.reader.prefs_reopen_pending) {
        navigation_wait_stage = ReaderPagePreparationStage();
        navigation_wait = page_work_busy ? "page_worker" : BookFontReloadBusy() ? "font"
                          : st.reader.page_image_busy.load() ? "image" : "open_or_layout";
        return;
    }
    if (st.reader.layout_busy.load()) {
        navigation_wait = "layout";
        return;
    }
    std::unique_ptr<ReaderPageResult> result;
    if (pending_kind == BookNavKind::kDelta) {
        const auto found = std::find_if(prefetched.begin(), prefetched.end(),
                                        [](const auto& page) { return page->value == pending_value; });
        if (found != prefetched.end()) {
            result = std::move(*found);
            prefetched.erase(found);
        }
    }
    const bool cached = result != nullptr;
    if (!cached) {
        result.reset(new (std::nothrow) ReaderPageResult{});
    }
    if (!result) {
        ESP_LOGE(kTag, "page result allocation failed");
        pending = false;
        return;
    }
    int value = pending_value;
    if (!cached) {
        result->cursor = st.reader.session->CreateNavigationCursor(pending_kind, value);
    }
    if (!result->cursor) {
        pending = false;
        ESP_LOGE(kTag, "navigation snapshot failed");
        return;
    }
    result->kind = pending_kind;
    result->value = value;
    result->prefetch = false;
    result->token = generation;
    result->started_us = requested_us;
    const bool pixels_ready = cached && !result->raster.empty();
    page_work_generation = generation;
    if (cached) {
        result->queue_us = esp_timer_get_time() - requested_us;
        result->navigation_us = result->raster_us = 0;
        result->chapter_only = false;
    }
    if (!pixels_ready && !StartReaderPageWork(std::move(result))) {
        page_work_busy = false;
        pending = false;
        return;
    }
    pending = false;
    page_work_busy = !pixels_ready;
    if (pixels_ready) {
        PublishReaderPage(std::move(result));
    }
}

bool ReaderPageWorkPending() {
    return pending || page_work_busy;
}

std::unique_ptr<ReaderPageResult> TakeReaderPageResult() {
    return std::move(ready);
}

void CancelReaderPageWork() {
    HideChapterLoadingHint();
    ++generation;
    pending = false;
    pending_direction = 0;
    ready.reset();
    CancelReaderPagePreparation();
    InvalidateReaderPagePrefetch();
    idle_after_us = 0;
}

void InvalidateReaderPagePrefetch() {
    std::lock_guard<std::recursive_mutex> cache_lock(prefetch_mutex);
    std::vector<std::unique_ptr<ReaderPageResult>>().swap(prefetched);
    prefetch_failed = 0;
    if (prefetching) {
        discard_prefetch = true;
        CancelReaderPagePreparation();
    }
}
