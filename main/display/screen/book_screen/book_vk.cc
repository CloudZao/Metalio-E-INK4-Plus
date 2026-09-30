#include "book_screen/book_vk.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/reader/book_reader_prefs.h"

#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>

#include "assistant_screen/assistant_screen.h"
#include "book_screen/book_screen.h"
#include "display_orient.h"
#include "lv_adapter_display.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"

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
    AssistantScreen::RequestOpen();
}

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
        RequestRenderBookshelfPage();
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
        const int pages =
            std::max(1, (st.reader.session->TocCount() + st.reader.toc_page_size - 1) /
                            std::max(1, st.reader.toc_page_size));
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

    // 正文：±10 写入积压，Apply 一次吃到目标页（或边界）
    if (!QueueReaderPageTurn(page_delta)) {
        return false;
    }
    return page_delta < 0 ? st.reader.session->HasPrevPage() : st.reader.session->HasNextPage();
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
    // 盖板 vk_home 长按不进百问（仅 BOOT）；交默认策略一键回系统首页
    if (std::strcmp(key_name, "vk_home") == 0) {
        return false;
    }
    return VkPageRepeatTryStart(key_name, BookPageRepeatStep);
}

bool BookScreen::OnVkKeyPressUp(const char* key_name) {
    if (!VkPageRepeatOnPressUp(key_name)) {
        return false;
    }
    // 松手时若还有未画的 delta，补一帧（通常定时器已停、积压很小）
    if (s_reader_page_delta.load(std::memory_order_acquire) != 0) {
        RequestRenderReaderPage();
    }
    return true;
}

bool BookScreen::OnVkKey(const char* key_name) {
    if (key_name == nullptr) {
        return true;
    }
    const char* screen = VkKey_ActiveScreen();
    auto& st = Book_State();

    if (std::strcmp(screen, kScreenRead) == 0) {
        // 正文 vk_home 短按：设置/目录浮层→收起；长按由 VkKey 默认回系统首页
        if (std::strcmp(key_name, "vk_home") == 0) {
            if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
                BookLvAsync(AsyncHideReadChrome);
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
            // 悬浮设置：分区配置 → 字体多选 → 收起；next 忽略
            // 分区面板有 lv_obj_clean/del：须 BookLvAsync，禁止 touch_feed 同步改树
            if (std::strcmp(key_name, "vk_prev") == 0) {
                if (st.tap.tap_zone_ui == BookUiState::TapZoneUi::kPick) {
                    BookLvAsync(AsyncTapZonePickBack);
                } else if (st.tap.tap_zone_ui != BookUiState::TapZoneUi::kClosed) {
                    BookLvAsync(AsyncTapZonesPanelClose);
                } else if (st.settings.font_multi) {
                    ExitFontMultiMode(true);
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
            }
            return true;
        }
        // 目录列表：vk 翻列表页
        if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
            const int pages =
                std::max(1, (st.reader.session->TocCount() + st.reader.toc_page_size - 1) /
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
                BookLvAsync(StartReadAsync,
                            reinterpret_cast<void*>(static_cast<intptr_t>(st.shelf.selected)));
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
            return false; // 阅读首页：出栈回系统首页
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            BookLvAsync(OpenBookshelfAsync);
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenBookshelf) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            BookLvAsync(BackToReadingHomeAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            if (st.shelf.list_page > 0) {
                --st.shelf.list_page;
                RequestRenderBookshelfPage();
                return true;
            }
            // 列表模式：子目录先退回上层（扫盘禁止在 touch_feed 同步做）
            if (st.shelf.shelf_view == kBookReaderShelfViewList && !ShelfDirIsRoot()) {
                st.shelf.shelf_dir = ShelfParentDir(st.shelf.shelf_dir);
                st.shelf.list_page = 0;
                BookLvAsync(ShelfReloadAndRenderAsync);
                return true;
            }
            // 第一页：多选中则取消批量，否则回阅读首页
            if (st.shelf.shelf_multi) {
                ExitShelfMultiMode(true);
                return true;
            }
            BookLvAsync(BackToReadingHomeAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            if (st.shelf.list_page + 1 < Book_ListPageCount()) {
                ++st.shelf.list_page;
                RequestRenderBookshelfPage();
            }
            return true;
        }
        return true;
    }
    return true;
}

void BookScreen::OnEnterStandby() {
    PauseReadingTimeForStandby();
    // 浅睡会卸 SD：先关 epdfont/书文件句柄，避免醒后 FILE* 悬空 → 翻页全白
    auto& st = Book_State();
    if (st.reader.session && st.reader.session->IsOpen()) {
        ReleaseBookFont();
        st.reader.session->DetachSdHandles();
    }
}

void BookScreen::OnResumeFromStandby() {
    ResumeReadingTimeAfterStandby();
    auto& st = Book_State();
    if (st.reader.session && st.reader.session->IsOpen()) {
        EnsureBookFont();
        st.reader.session->ReattachSdHandles();
        st.reader.session->SetFont(BookFont());
    }
    if (st.reader.read_scr == nullptr || !lv_obj_is_valid(st.reader.read_scr)) {
        return;
    }
    RequestRenderReaderPage();
    if (DisplayUiSetOrient(BookReaderPrefsOrient())) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestNextFullRefresh();
        }
    }
}

