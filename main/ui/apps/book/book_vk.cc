#include <algorithm>
#include <cstring>
#include <optional>
#include <esp_log.h>
#include <lvgl.h>
#include "business/conversation/conversation_presentation.h"
#include "ui/apps/book/book_vk.h"
#include "boards/common/storage/sd_access.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ttf_convert.h"
#include "ui/apps/assistant/assistant_screen.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/reader/book_open_worker.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_page_image.h"
#include "ui/apps/book/reader/book_reader_orient.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_actions.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/apps/book/shelf/book_home.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

bool BookScreen::IsReadScreenActive() {
    return std::strcmp(VkKey_ActiveScreen(), kScreenRead) == 0;
}

bool BookScreen::IsReadingActive() {
    // 仅正文沉浸翻页；目录/设置浮层仍走音量。编排中仍算阅读中，以便音量键走翻页提示。
    if (!IsReadScreenActive()) {
        return false;
    }
    auto& st = Book_State();
    return !IsReadOverlayChrome(st.reader.read_chrome);
}

static bool CancelBookBatchForKey(const char* key) {
    if (std::strcmp(key, "vk_prev") != 0 && std::strcmp(key, "vk_home") != 0) {
        return false;
    }
    const char* screen = VkKey_ActiveScreen();
    const auto& st = Book_State();
    if (std::strcmp(screen, kScreenBookshelf) == 0 && (st.shelf.shelf_multi || ShelfDeleteBusy())) {
        BookLvAsync([](void*) { OnShelfMultiCancel(nullptr); });
        return true;
    }
    if (std::strcmp(screen, kScreenRead) == 0 && st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        if (st.ttf.ttf_multi) {
            BookLvAsync([](void*) {
                Book_State().ttf.ttf_selected.clear();
                Book_State().ttf.ttf_multi = false;
                RebuildSettingsSheet();
            });
            return true;
        }
        if (st.settings.font_multi) {
            BookLvAsync([](void*) { ExitFontMultiMode(true); });
            return true;
        }
        if (st.settings.bookmark_multi) {
            BookLvAsync([](void*) {
                ResetBookmarkSelection();
                RequestSettingsSheetPaint();
            });
            return true;
        }
    }
    return false;
}

bool BookScreen::OnBootClick() {
    // 仅正文阅读屏：BOOT 短按切换悬浮排版卡片（弹出/收起）；其余页面不消费。
    if (std::strcmp(VkKey_ActiveScreen(), kScreenRead) != 0) {
        return false;
    }
    BookLvAsync(ToggleReadSettingsSheetAsync);
    return true;
}

bool BookScreen::OnBootLongPress() {
    RequestOpenAssistantFromBook();
    return true;
}

bool BookScreen::OnVkKeyLongPress(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    if (CancelBookBatchForKey(key_name)) {
        return true;
    }
    // 与 397 一致：先退出阅读并恢复竖屏，再回系统首页。
    if (std::strcmp(key_name, "vk_home") == 0) {
        if (!ScreenLvAsyncUrgent(GoHomeFromBookAsync)) {
            RequestBackHome();
        }
        return true;
    }
    if (std::strcmp(VkKey_ActiveScreen(), kScreenBookshelf) == 0 && HandleShelfSearchKey(key_name)) {
        return true;
    }
    if (BookmarkMessageVisible()) {
        return true;
    }
    return VkPageRepeatTryStart(key_name, BookPageRepeatStep);
}

bool BookScreen::OnVkKeyPressUp(const char* key_name) {
    if (!VkPageRepeatOnPressUp(key_name)) {
        return false;
    }
    return true;
}

bool BookScreen::OnPageKey(const char* key_name, bool long_press) {
    const char* screen = VkKey_ActiveScreen();
    auto& st = Book_State();
    const bool shelf = std::strcmp(screen, kScreenBookshelf) == 0;
    const bool reader = std::strcmp(screen, kScreenRead) == 0 &&
                        st.reader.read_chrome != BookUiState::ReadChrome::kSettings;
    if (!shelf && !reader) {
        return false;
    }
    if (BookmarkMessageVisible() || (shelf && (st.shelf.shelf_multi || ShelfDeleteBusy()))) {
        return true;
    }
    if (long_press) {
        VkPageRepeatTryStart(key_name, BookPageRepeatStep);
    } else {
        if (reader) {
            ReaderAutoTurnManualInput();
        }
        BookPageRepeatStep(std::strcmp(key_name, "vk_prev") == 0 ? -1 : 1);
    }
    return true;
}

bool BookScreen::OnVkKey(const char* key_name) {
    if (key_name == nullptr) {
        return true;
    }
    const char* screen = VkKey_ActiveScreen();
    auto& st = Book_State();
    if (CancelBookBatchForKey(key_name)) {
        return true;
    }
    if (std::strcmp(VkKey_ActiveScreen(), kScreenBookshelf) == 0 && HandleShelfSearchKey(key_name)) {
        return true;
    }
    if (BookmarkMessageVisible()) {
        if (std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_home") == 0) {
            BookLvAsync([](void*) { CloseBookmarkMessage(); });
        }
        return true;
    }

    if (std::strcmp(screen, kScreenRead) == 0) {
        if (std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_next") == 0) {
            ReaderAutoTurnManualInput();
        }
        // 正文 vk_home 短按：设置/目录浮层→收起；长按经阅读退出清理回系统首页
        if (std::strcmp(key_name, "vk_home") == 0) {
            if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
                if (st.settings.reader_tab == 3 && st.settings.reader_settings_page != 0) {
                    BookLvAsync([](void*) { SelectReadingSheetTab(3); });
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
                return true;
            }
            BookLvAsync(BackToDetailAsync);
            return true;
        }
        if (st.reader.opening.load()) {
            return true;  // 解析中：翻页键无效；HOME 已在上方处理
        }
        if (!st.reader.session || !st.reader.session->IsOpen()) {
            // 打开失败页：上一页回详情
            if (std::strcmp(key_name, "vk_prev") == 0) {
                BookLvAsync(BackToDetailAsync);
            }
            return true;
        }
        if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
            // 悬浮设置：子页回阅读设置首页；next 忽略
            if (std::strcmp(key_name, "vk_prev") == 0) {
                if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) {
                    BookLvAsync([](void*) {
                        if (Book_State().ttf.ttf_mode == BookUiState::TtfMode::kList) {
                            SelectReadingSheetTab(1);
                        } else {
                            ReturnToTtfList();
                        }
                    });
                } else if (st.settings.reader_tab == 3 && st.settings.reader_settings_page != 0) {
                    BookLvAsync([](void*) { SelectReadingSheetTab(3); });
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
            }
            return true;
        }
        // 目录列表：vk 翻列表页
        if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
            const int pages = std::max(1, (st.reader.session->TocCount() + st.reader.toc_page_size - 1) /
                                              std::max(1, st.reader.toc_page_size));
            if (std::strcmp(key_name, "vk_prev") == 0) {
                if (st.reader.toc_list_page > 0) {
                    --st.reader.toc_list_page;
                    RequestRenderTocList();
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
                return true;
            }
            if (std::strcmp(key_name, "vk_next") == 0) {
                if (st.reader.toc_list_page + 1 < pages) {
                    ++st.reader.toc_list_page;
                    RequestRenderTocList();
                }
                return true;
            }
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            if (!QueueReaderPageTurn(-1)) {
                BookLvAsync(BackToDetailAsync);
            }
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            QueueReaderPageTurn(1);
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenDetail) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            BookLvAsync(BackToLibraryAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            BookLvAsync(BackToLibraryAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            if (st.reader.opening.load()) {
                ESP_LOGW(TAG, "detail vk_next start ignored: opening=1 open_token=%u sel=%d",
                         static_cast<unsigned>(st.reader.open_token.load()), st.shelf.selected);
            } else if (ReaderWorkersBusy()) {
                WarnReaderWorkersBusy("detail vk_next");
            } else {
                BookLvAsync(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(st.shelf.selected)));
            }
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenLibrary) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            RequestBackHome();
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            return false;  // 阅读首页：出栈回系统首页
        }
        return true;
    }

    if (std::strcmp(screen, kScreenBookshelf) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            if (!ShelfDirIsRoot()) {
                st.shelf.shelf_dir = ShelfParentDir(st.shelf.shelf_dir);
                st.shelf.list_page = 0;
                BookLvAsync(ShelfReloadAndRenderAsync);
            } else {
                BookLvAsync(BackToReadingHomeAsync);
            }
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            if (st.shelf.list_page > 0) {
                --st.shelf.list_page;
                RequestRenderBookshelfPage(true);
                return true;
            }
            // 列表模式：子目录先退回上层（扫盘禁止在 touch_feed 同步做）
            if (!ShelfDirIsRoot()) {
                st.shelf.shelf_dir = ShelfParentDir(st.shelf.shelf_dir);
                st.shelf.list_page = 0;
                BookLvAsync(ShelfReloadAndRenderAsync);
                return true;
            }
            BookLvAsync(BackToReadingHomeAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            if (st.shelf.list_page + 1 < Book_ListPageCount()) {
                ++st.shelf.list_page;
                RequestRenderBookshelfPage(true);
            }
            return true;
        }
        return true;
    }
    return true;
}

static bool s_ttf_return_after_standby = false;                   // 导入页待机后返回 TTF 文件列表
static std::optional<SdAccessTransaction> s_reader_standby_hold;  // 资源收尾完成前阻止 SD 卸卡

void BookScreen::OnEnterStandby() {
    StopReaderAutoOrient();
    s_ttf_return_after_standby |= Book_State().ttf.ttf_mode != BookUiState::TtfMode::kNone;
    TtfPanelClose();
    CancelReadFontScan();
    CloseBookmarkMessage();
    ResetBookmarkSelection();
    PauseReadingTimeForStandby();
    auto& st = Book_State();
    if (st.reader.standby_paused) {
        return;
    }
    CancelReaderPageWork();
    st.reader.standby_paused = true;
    s_reader_standby_hold.emplace();
    st.reader.layout_token.fetch_add(1);
    CancelPageImageLoad();
    // 打开任务独占 session，先让其完成，避免同时修改尚未建好的文档状态。
    if (st.reader.session && !st.reader.opening.load()) {
        st.reader.session->SetBackgroundPaused(true);
    }
    st.reader.layout_again =
        st.reader.layout_again || st.reader.layout_debounce_timer != nullptr || BookFontReloadBusy();
    st.reader.layout_reload_font =
        st.reader.layout_reload_font || st.reader.layout_debounce_reload_font || BookFontReloadBusy();
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    ScreenPaintCoalesceReset(&s_reader_paint);
    ScreenPaintCoalesceReset(&s_toc_paint);
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
    PollStandbyWork();
}

void BookScreen::PollStandbyWork() {
    BookApplyDeferredOpenResult();
    auto& st = Book_State();
    if (st.reader.powering_off || !st.reader.standby_paused || ReaderWorkersBusy() || TtfConvertBusy()) {
        return;
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
    }
    if (StandbyPresentation::IsActive()) {
        // 壁纸加载提示仍需绘制底层页面；内容就绪后才允许关闭其字体资源。
        if (!StandbyPresentation::IsPaintReady()) {
            return;
        }
        if (!st.reader.standby_detached) {
            if (st.reader.session) {
                st.reader.session->DetachSdHandles();
            }
            ReleaseBookFont(false);
            st.reader.standby_detached = true;
            s_reader_standby_hold.reset();
        }
        return;
    }
    if (st.reader.session && st.reader.session->IsOpen()) {
        EnsureBookFont();
        if (st.reader.standby_detached) {
            st.reader.session->ReattachSdHandles();
        }
        st.reader.session->SetFont(BookLayoutFont(), BookFont());
        st.reader.session->SetBackgroundPaused(false);
    }
    st.reader.standby_paused = st.reader.standby_detached = false;
    BookApplyDeferredOpenResult();
    const bool return_to_ttf = s_ttf_return_after_standby;
    s_ttf_return_after_standby = false;
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings && st.settings.reader_tab == 1) {
        StartReadFontScan();
    }
    s_reader_standby_hold.reset();
    if (st.reader.deferred_cleanup || !st.reader.session || !st.reader.session->IsOpen() || !st.reader.read_scr ||
        !lv_obj_is_valid(st.reader.read_scr)) {
        return;
    }
    ResumeReadingTimeAfterStandby();
    UpdateReadFooter();
    if (DisplayUiSetOrient(ReaderDisplayOrient())) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestFullOnNextCommit();
        }
    }
    ApplyReaderChromeSize();
    SyncReaderAutoOrient();
    RequestRenderReaderPage();
    if (return_to_ttf && st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        ReturnToTtfList();
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        RequestSettingsSheetPaint();
    }
}

void BookScreen::OnResumeFromStandby() {
    PollStandbyWork();
}

void RequestOpenAssistantFromBook() {
    const char* screen = VkKey_ActiveScreen();
    if (screen != nullptr && std::strcmp(screen, kScreenRead) == 0) {
        ESP_LOGI(TAG, "AI long (%s) -> release session, assistant", screen);
        RequestOpenAssistantFromReader();
        return;
    }
    CancelDetailCoverLoad();
    CancelListCoverFill();
    ESP_LOGI(TAG, "AI long (%s) -> assistant", screen != nullptr ? screen : "?");
    ConversationPresentation::RequestOpen();
}

bool BookPageRepeatStep(int page_delta) {
    const char* screen = VkKey_ActiveScreen();
    auto& st = Book_State();
    if (page_delta == 0) {
        return false;
    }

    if (std::strcmp(screen, kScreenBookshelf) == 0) {
        const int last = std::max(0, Book_ListPageCount() - 1);
        int next = st.shelf.list_page + page_delta;
        if (next < 0) {
            next = 0;
        } else if (next > last) {
            next = last;
        }
        if (next == st.shelf.list_page) {
            return false;
        }
        st.shelf.list_page = next;
        RequestRenderBookshelfPage(true);
        return page_delta < 0 ? st.shelf.list_page > 0 : st.shelf.list_page < last;
    }

    if (std::strcmp(screen, kScreenLibrary) == 0) {
        return false;
    }

    if (std::strcmp(screen, kScreenRead) != 0 || st.reader.opening.load() || !st.reader.session ||
        !st.reader.session->IsOpen()) {
        return false;
    }

    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        const int pages = std::max(
            1, (st.reader.session->TocCount() + st.reader.toc_page_size - 1) / std::max(1, st.reader.toc_page_size));
        int next = st.reader.toc_list_page + page_delta;
        const int last = std::max(0, pages - 1);
        if (next < 0) {
            next = 0;
        } else if (next > last) {
            next = last;
        }
        if (next == st.reader.toc_list_page) {
            return false;
        }
        st.reader.toc_list_page = next;
        RequestRenderTocList();
        return page_delta < 0 ? st.reader.toc_list_page > 0 : st.reader.toc_list_page < last;
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        // 悬浮设置卡片：长按连翻不翻页
        return false;
    }

    if (!QueueReaderPageTurn(page_delta)) {
        return false;
    }
    return page_delta < 0 ? st.reader.session->HasPrevPage() : st.reader.session->HasNextPage();
}
