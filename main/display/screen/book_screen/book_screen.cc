#pragma GCC optimize("O1")

#include "book_screen/book_screen.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/book_vk.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <atomic>
#include <string>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "screen_common.h"
#include "vk_key_handler.h"

ScreenPaintCoalesce s_reader_paint{};
ScreenPaintCoalesce s_library_paint{};
ScreenPaintCoalesce s_shelf_check_paint{};
ScreenPaintCoalesce s_toc_paint{};
ScreenPaintCoalesce s_settings_sheet_paint{}; // 设置卡：连点只记状态，抬起后一次刷控件
// 盖板键/长按定时器只累加；真正 Next/Prev 在 LVGL 绘制里做，避免跨章重建 pages_ 时 UAF
std::atomic<int> s_reader_page_delta{0};

BookUiState& Book_State() {
    static BookUiState s;
    return s;
}

lv_obj_t* BookScreen::Create() {
    auto& st = Book_State();
    const bool workers_busy = st.reader.opening.load() || st.reader.layout_busy.load() ||
                              st.reader.chapter_pages_busy.load() || st.reader.page_image_busy.load();
    if (workers_busy) {
        st.reader.open_token.fetch_add(1);
        st.reader.layout_token.fetch_add(1);
        st.reader.chapter_pages_token.fetch_add(1);
        CancelPageImageLoad();
        if (st.reader.session) {
            st.reader.session->InvalidateChapterPageTable();
            st.reader.session->AbortTxtPaginate();
        }
        StopReadTimeCheckpointTimer();
        st.reader.deferred_cleanup = true;
    } else {
        // 收尾：含 worker 投递失败后留下的 deferred_cleanup
        if (st.reader.deferred_cleanup) {
            FinishDeferredCleanup();
        } else {
            EndReadingTimeTracking();
            st.reader.session.reset();
            ReleaseBookFont();
        }
    }
    // 封面由详情屏 DELETE 释放；此处不 Reset，避免旧屏仍引用时 UAF
    st.reader.read_scr = nullptr;
    st.reader.content = nullptr;
    st.reader.footer_host = nullptr;
    st.reader.page_label = nullptr;
    st.reader.title_label = nullptr;
    st.reader.status_label = nullptr;
    st.reader.status_bar = nullptr;
    st.reader.status_overlay = nullptr;
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
    st.reader.layout_again = false;
    st.reader.layout_reload_font = false;
    st.reader.layout_anchor = {};
    st.reader.chapter_pages_again = false;
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    StopFooterClockTimer();
    st.reader.layout_hint_done_until_us = 0;
    st.reader.open_bar = nullptr;
    st.reader.open_pct_lbl = nullptr;
    st.shelf.shelf_multi = false;
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.multi_bar = nullptr;
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    CancelListCoverFill();
    st.shelf.recent_covers.clear();
    st.shelf.shelf_covers.clear();

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Book_DisableScroll(scr);
    st.shelf.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.shelf.list_scr == tgt) {
                CancelListCoverFill();
                s.shelf.list_scr = nullptr;
                s.shelf.list_body = nullptr;
                s.shelf.list_footer = nullptr;
                s.shelf.multi_bar = nullptr;
                s.shelf.shelf_multi = false;
                s.shelf.shelf_selected.clear();
                s.shelf.recent_covers.clear();
                s.shelf.shelf_covers.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_label = status.status_label;
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, kListPad, 0);
    lv_obj_set_style_pad_bottom(body, kListPad + kHomeBottomPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kHomeGap, 0);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, Book_ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, 0, 0);
    Book_DisableScroll(list_body);
    st.shelf.list_body = list_body;
    st.shelf.list_footer = nullptr;
    st.shelf.multi_bar = nullptr;

    if (!SdCardManager::GetInstance().IsMounted()) {
        Book_ShowMessage(list_body, Lang::Strings::BOOK_NO_SD, Book_ListFont());
        VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
        return scr;
    }

    st.shelf.books.clear();
    // 优先复用开机/上次扫库缓存，未命中再扫盘
    reader::LoadBookLibrary(reader::kDefaultBooksDir, st.shelf.books);
    // 仅丢掉已不在书库的 miss；保留仍在库中的，避免回首页再进又重复 Open 抽封面
    if (!st.shelf.cover_embed_miss.empty()) {
        st.shelf.cover_embed_miss.erase(
            std::remove_if(st.shelf.cover_embed_miss.begin(), st.shelf.cover_embed_miss.end(),
                           [&](const std::string& path) {
                               for (const auto& b : st.shelf.books) {
                                   if (b.path == path) {
                                       return false;
                                   }
                               }
                               return true;
                           }),
            st.shelf.cover_embed_miss.end());
    }
    RenderReadingHome();

    // factory=Create：进百问后返回阅读首页
    VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
    return scr;
}
