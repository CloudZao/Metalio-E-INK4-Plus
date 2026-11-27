#include "assets/lang_reader.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/book/reader/book_page_work.h"

#pragma GCC optimize("O1")

#include <cstring>
#include <memory>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "assets/lang_config.h"
#include "business/reader/page_text_raster.h"
#include "business/reader/reader.h"
#include "business/reader/reader_page_render.h"
#include "business/reader/text_encoding.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/book_vk.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_open_worker.h"
#include "ui/apps/book/reader/book_page_image.h"
#include "ui/apps/book/reader/book_reader_orient.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/shelf/book_detail.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

static bool s_reader_first_paint = false;

static void FinishReaderFirstPaintGate() {
    if (!s_reader_first_paint) {
        return;
    }
    s_reader_first_paint = false;
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestFullOnNextCommit();
    }
    auto& st = Book_State();
    if (st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr)) {
        lv_obj_invalidate(st.reader.read_scr);
    }
}

void OnReaderUnderlineDraw(lv_event_t* e) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (label == nullptr) {
        return;
    }
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(label)));
    if (mode != kBookReaderUnderlineSolid && mode != kBookReaderUnderlineDashed) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (layer == nullptr || font == nullptr || font->line_height <= 0) {
        return;
    }

    const char* txt = lv_label_get_text(label);
    if (!txt || !*txt) {
        return;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(txt);
    size_t left = std::strlen(txt);
    const int letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int width = 0, ink_bottom = -1;
    // 正文和分行章名均为单行 label；同次字形查询同时取得宽度与最低墨迹边界。
    while (left > 0) {
        uint32_t cp = 0, next = 0;
        const size_t n = Utf8Next(p, left, &cp);
        if (!n || cp == '\n') {
            break;
        }
        p += n;
        left -= n;
        Utf8Next(p, left, &next);
        lv_font_glyph_dsc_t glyph{};
        lv_font_get_glyph_dsc(font, &glyph, cp, next);
        width += glyph.adv_w + letter_space;
        if (glyph.box_w > 0 && glyph.box_h > 0) {
            ink_bottom = std::max(ink_bottom, ReaderGlyphInkBottom(font, glyph));
        }
    }
    width -= letter_space;
    if (width <= 0 || ink_bottom < 0) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_content_coords(label, &coords);
    if (IsJustifiedTextLabel(label) || IsCompactTextLabel(label)) {
        width =
            MakeTextLineSpacing(font, txt, lv_area_get_width(&coords), IsJustifiedTextLabel(label), false, letter_space)
                .width;
    }
    int x = coords.x1;
    const auto align = lv_obj_get_style_text_align(label, LV_PART_MAIN);
    if (align == LV_TEXT_ALIGN_CENTER) {
        x += (lv_area_get_width(&coords) - width) / 2;
    } else if (align == LV_TEXT_ALIGN_RIGHT) {
        x += lv_area_get_width(&coords) - width;
    }
    const int y =
        coords.y1 + ReaderUnderlineTop(ink_bottom) + BookReaderPrefsTextValue(BookTextOption::UnderlineOffset);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = UiColorForeground();
    dsc.bg_opa = LV_OPA_COVER;
    const int step = mode == kBookReaderUnderlineDashed ? 10 : width;
    const int segment = mode == kBookReaderUnderlineDashed ? 6 : width;
    const int end_x = std::min<int>(x + width - 1, coords.x2);
    for (; x <= end_x; x += step) {
        lv_area_t area = {x, y, std::min(x + segment - 1, end_x), y + kReaderUnderlineThickness - 1};
        lv_draw_rect(layer, &dsc, &area);
    }
}

void AttachReaderUnderline(lv_obj_t* label, int mode) {
    if (label == nullptr || mode == kBookReaderUnderlineOff) {
        return;
    }
    lv_obj_set_user_data(label, reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
    lv_obj_add_event_cb(label, OnReaderUnderlineDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
}

lv_obj_t* CreateReaderScreen(const BookInfo& info) {
    auto& st = Book_State();
    // 硬护栏：layout/open worker 仍用旧 session 时禁止 make_unique 拆掉（UAF）
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("CreateReaderScreen");
        return BookScreen::Create();
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
        if (st.reader.deferred_cleanup) {
            return BookScreen::Create();
        }
    }
    s_reader_first_paint = true;
    StopReaderAutoOrient();
    DisplayUiSetOrient(ReaderDisplayOrient());
    // 仅打开正文时加载 SD 字体；界面控件用固件字
    EnsureBookFont();
    CancelReadFontScan();
    // 换新 session 前停旧 timer/墙钟，避免回调 UAF 或漏写本段秒数
    if (!RetireReaderSession()) {
        st.reader.deferred_cleanup = true;
        return BookScreen::Create();
    }
    st.reader.session = std::make_unique<BookSession>();
    BindSessionLayoutPrefs(*st.reader.session);
    st.reader.opening_format = info.format;

    st.reader.background_layer = nullptr;
    st.reader.background_image.reset();
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    Book_DisableScroll(scr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_bar = status.bar;
    st.reader.status_overlay = status.overlay;
    st.reader.status_h = status.height;
    st.reader.status_label = status.status_label;
    if (status.status_label) {
        lv_label_set_text(status.status_label, "");
        lv_obj_add_flag(status.status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    // 沉浸阅读：默认藏顶栏，正文占满状态栏区域
    SetReadStatusVisible(false);

    st.reader.title_label = nullptr;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - ReaderFooterHeight());
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    Book_DisableScroll(body);
    AttachReadBodyInput(body);
    st.reader.content = body;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    st.reader.toc_list_page = 0;

    // 底部阅读浮层（默认隐藏；菜单打开后显示时长页）
    lv_obj_t* sheet = lv_obj_create(scr);
    st.settings.settings_sheet = sheet;
    lv_obj_remove_style_all(sheet);
    lv_obj_set_width(sheet, LV_HOR_RES - kSheetSidePad * 2);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, (st.reader.status_h > 0 ? st.reader.status_h : 40) + 10);
    lv_obj_set_style_bg_color(sheet, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sheet, kSheetBorderW, 0);
    lv_obj_set_style_border_color(sheet, UiColorForeground(), 0);
    lv_obj_set_style_radius(sheet, kSheetRadius, 0);
    lv_obj_set_style_pad_all(sheet, kSheetInnerPad, 0);
    lv_obj_set_style_pad_row(sheet, kSheetGap, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    Book_DisableScroll(sheet);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    // 点在卡片空白处不穿透到正文（避免误收）
    lv_obj_add_event_cb(sheet, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    // 阅读根容器内上下紧接，底栏固定高度，信息标签在槽内居中。
    lv_obj_t* foot_host = lv_obj_create(scr);
    lv_obj_remove_style_all(foot_host);
    lv_obj_set_size(foot_host, LV_HOR_RES, ReaderFooterHeight());
    lv_obj_align(foot_host, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot_host, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(foot_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(foot_host, 0, 0);
    Book_DisableScroll(foot_host);
    lv_obj_clear_flag(foot_host, LV_OBJ_FLAG_CLICKABLE);
    st.reader.footer_host = foot_host;

    lv_obj_t* footer = lv_label_create(foot_host);
    lv_obj_set_width(footer, LV_HOR_RES - UiSx(16));
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(footer, UiColorForeground(), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(footer, "");
    lv_obj_center(footer);
    Book_DisableScroll(footer);
    st.reader.page_label = footer;

    auto make_info_label = [](lv_obj_t* parent) {
        auto* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(label, UiColorForeground(), 0);
        lv_obj_set_height(label, Book_ItemFont()->line_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, "");
        Book_DisableScroll(label);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        return label;
    };
    st.reader.info_pages = make_info_label(foot_host);
    auto* info_header = lv_obj_create(scr);
    lv_obj_remove_style_all(info_header);
    lv_obj_set_style_bg_color(info_header, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(info_header, LV_OPA_COVER, 0);
    Book_DisableScroll(info_header);
    lv_obj_clear_flag(info_header, LV_OBJ_FLAG_CLICKABLE);
    st.reader.info_header = info_header;
    st.reader.info_title = make_info_label(info_header);
    st.reader.info_progress = make_info_label(info_header);

    // 信息栏位于正文上方，设置和系统状态栏继续覆盖信息栏。
    lv_obj_move_foreground(sheet);
    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);

    ShowOpenProgress(0);
    st.reader.read_scr = scr;
    BookBindPageLifecycle(scr, true);
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == Book_State().reader.read_scr) {
                StopReaderAutoOrient();
            }
        },
        LV_EVENT_DELETE, nullptr);
    SyncReaderAutoOrient();
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    ApplyReaderChromeSize();
    SyncFooterClockTimer();
    st.reader.session->SetViewport(st.reader.viewport_w, st.reader.viewport_h);
    ScreenSetIsHome(false);
    // factory=ResumeReader：进百问压栈后返回依 .pos 续读；短按 HOME 回详情
    VkKey_AttachScreen(scr, kScreenRead, BookAiLongPressDesc(ResumeReaderScreen));
    StartOpenWorker(st.shelf.selected);
    return scr;
}

void RenderReaderPage() {
    if (Book_State().reader.standby_paused) {
        return;
    }

    // Start a fresh interval after every render, including manual turns and layout changes.
    struct AutoTurnResetOnExit {
        ~AutoTurnResetOnExit() {
            if (ReaderAutoTurnEnabled()) {
                ResetReaderAutoTurnDeadline();
            }
        }
    } auto_turn_reset;

    if (ConsumeReaderAutoTurnRequest()) {
        RequestReaderNavigation(BookSession::NavigationKind::kDelta, 1);
    }
    auto& st = Book_State();
    if (st.reader.content == nullptr || !st.reader.session) {
        s_reader_first_paint = false;
        return;
    }
    // Loading/errors/TOC stay binary; opt into text gray only when a page exists.
    EpdSetContentRole(st.reader.content, EpdContentRole::Monochrome);
    st.reader.open_bar = nullptr;
    st.reader.open_pct_lbl = nullptr;
    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        s_reader_first_paint = false;
        SyncReaderAaForImmersion(false);
        RenderTocList();
        return;
    }
    // 设置卡片盖在正文上：重绘正文时保持卡片；其它情况回到沉浸
    const bool settings_overlay = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    SyncReaderAaForImmersion(true);
    if (!settings_overlay) {
        st.reader.read_chrome = BookUiState::ReadChrome::kReading;
        SetReadStatusVisible(false);
        SetReadSettingsSheetVisible(false);
    }
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    // 目录等会把 content 设成 flex；正文用绝对坐标居中，必须关掉 layout
    lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_NONE, 0);

    const bool page_images = st.reader.session->IsPageImagesMode();
    const lv_coord_t vw = st.reader.session->ViewportWidth();
    const lv_coord_t vh = st.reader.session->ViewportHeight();
    const bool viewport_changed = vw != st.reader.viewport_w || vh != st.reader.viewport_h;
    const bool chapter_relayout = st.reader.session->LiveRelayoutPending() && !st.reader.session->StructurePending();
    // 分页视口与当前几何不一致：重排（宽或高任一过期都会让末行挤进底栏 / 行宽偏短）
    if (st.reader.viewport_w > 40 && st.reader.viewport_h > 40 && vw > 40 && vh > 40 && viewport_changed &&
        !st.reader.opening.load() && !st.reader.layout_busy.load() && !chapter_relayout) {
        ESP_LOGW(TAG, "viewport mismatch session=%dx%d ui=%dx%d → relayout", static_cast<int>(vw), static_cast<int>(vh),
                 static_cast<int>(st.reader.viewport_w), static_cast<int>(st.reader.viewport_h));
        ApplyReaderLayoutLive(false);
        return;
    }

    // 视口尚未生效或章节仍在重排时，不能把旧页按新方向重新绘制。
    if (st.reader.layout_again || viewport_changed ||
        (st.reader.session->Info().format != BookFormat::kTxt && chapter_relayout &&
         !st.reader.session->NeedsCurrentPagePreview() && !st.reader.session->HasCurrentPagePreview())) {
        if (!ReaderWorkersBusy() && st.reader.session->Error() != BookReadError::kNone) {
            Book_ShowMessage(st.reader.content, LangString_BOOK_PAGE_LOAD_FAILED);
        } else {
            ShowReaderPageLoading();
        }
        return;
    }

    auto prepared = TakeReaderPageResult();
    if (!prepared) {
        RequestReaderNavigation(BookSession::NavigationKind::kRefresh);
        return;
    }

    struct RetiredFontRelease {
        ~RetiredFontRelease() {
            ReleaseRetiredBookFont();
        }
    } release_font;

    CancelPageImageLoad();
    const Page* page = st.reader.session->CurrentPageData();
    const bool reuse_raster =
        page && !page_images && BookReaderPrefsBackground() == 0 && PageIsTextOnly(*page) &&
        st.reader.text_raster_image && lv_obj_is_valid(st.reader.text_raster_image) &&
        lv_obj_get_child_count(st.reader.content) == 1 &&
        lv_obj_get_child_count(lv_obj_get_parent(lv_obj_get_parent(st.reader.text_raster_image))) == 1;
    if (!reuse_raster) {
        lv_obj_clean(st.reader.content);
        Book_DisableScroll(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    if (page == nullptr) {
        FinishReaderFirstPaintGate();
        Book_ShowMessage(st.reader.content, LangString_BOOK_PAGE_LOAD_FAILED);
        if (settings_overlay) {
            SetReadStatusVisible(true);
            SetReadSettingsSheetVisible(true);
        }
        return;
    }

    if (prepared->direction != 0) {
        const int effect = BookReaderPrefsPageTurn();
        EpdSetReaderEffect(effect == kBookReaderPageTurnWipe     ? EpdEffect::Wipe
                           : effect == kBookReaderPageTurnRipple ? EpdEffect::Ripple
                                                                 : EpdEffect::None);
        EpdPageTurnArea viewport;
        if (effect == kBookReaderPageTurnRipple) {
            const auto area = ReadContentArea();
            viewport = {area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area)};
        }
        EpdRequestPageTurn(prepared->direction, viewport);
    }
    EpdSetContentRole(st.reader.content,
                      !page_images && (BookReaderPrefsAntialias() || BookReaderPrefsBackground() == 3)
                          ? EpdContentRole::ReaderAntialias
                          : EpdContentRole::Monochrome);
    const int64_t t0 = esp_timer_get_time();

    const lv_font_t* book_font = BookFont();
    const lv_coord_t max_w = st.reader.viewport_w;
    const auto content_area = ReadContentArea();
    const lv_coord_t content_h = st.reader.viewport_h;
    const int background = page_images ? 0 : BookReaderPrefsBackground();
    lv_obj_set_style_text_color(st.reader.content, background == 3 ? lv_color_black() : UiColorForeground(), 0);
    const bool background_changed = st.reader.background_image != prepared->background;
    if (background_changed && st.reader.background_layer && lv_obj_is_valid(st.reader.background_layer)) {
        lv_obj_delete(st.reader.background_layer);
        st.reader.background_layer = nullptr;
    }
    st.reader.background_image = prepared->background;
    if (background_changed && prepared->background) {
        auto* image = prepared->background.get();
        image->BindDsc();
        auto* slot = lv_obj_create(st.reader.read_scr);
        lv_obj_remove_style_all(slot);
        lv_obj_set_size(slot, image->width, image->height);
        lv_obj_set_pos(slot, 0, 0);
        st.reader.background_layer = slot;
        lv_obj_move_background(slot);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(slot);
        auto* picture = lv_image_create(slot);
        lv_image_set_src(picture, &image->dsc);
        EpdSetContentRole(picture, EpdContentRole::ReaderAntialias);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(picture);
        auto* owner = new std::shared_ptr<RasterImage>(prepared->background);
        lv_obj_add_event_cb(
            picture,
            [](lv_event_t* e) { delete static_cast<std::shared_ptr<RasterImage>*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, owner);
    }
    st.reader.background_mode = background;
    st.reader.background_dark = prepared->dark;
    const int spacing_percent = st.reader.session->LineSpacingPercent();
    const lv_coord_t para_gap = st.reader.session->ParaGap();

    const auto attach_image = [&](lv_obj_t* parent, const PageItem& item, int col_w, int top) {
        const int img_h = item.image_h;
        lv_obj_t* slot = lv_obj_create(parent);
        lv_obj_remove_style_all(slot);
        lv_obj_set_width(slot, col_w);
        lv_obj_set_height(slot, page->items.size() == 1 ? content_h : img_h);
        if (top >= 0) {
            lv_obj_set_pos(slot, 0, top);
        }
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(slot);

        RasterImage cached;
        if (st.reader.session->TryCopyCachedPageImage(item.image_href, item.image_w, img_h, cached) &&
            !cached.empty()) {
            auto* heap_img = new RasterImage(std::move(cached));
            AttachPageImageToSlot(slot, heap_img, true);
        } else {
            lv_obj_t* label = lv_label_create(slot);
            lv_label_set_text(label, LangString_BOOK_LOADING);
            lv_obj_set_style_text_font(label, book_font, 0);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(label);
            BookUiState::PageImagePending pending;
            pending.href = item.image_href;
            pending.max_w = item.image_w;
            pending.max_h = img_h;
            pending.slot = slot;
            st.reader.page_image_pending.push_back(std::move(pending));
        }
    };

    // 正文统一预渲为 L8，插图按同一布局坐标异步填充。
    if (!page_images && book_font != nullptr && content_h > 40 && max_w > 40 && !prepared->raster.empty()) {
        const lv_coord_t col_w = prepared->column_width;
        const lv_coord_t col_x = content_area.x1 + (max_w - col_w) / 2;
        const int ink_h = prepared->ink_height;

        auto* heap_img = reuse_raster ? static_cast<RasterImage*>(lv_obj_get_user_data(st.reader.text_raster_image))
                                      : new RasterImage();
        if (reuse_raster) {
            lv_image_cache_drop(&heap_img->dsc);
            lv_image_header_cache_drop(&heap_img->dsc);
        }
        *heap_img = std::move(prepared->raster);
        lv_obj_t* slot = reuse_raster ? lv_obj_get_parent(st.reader.text_raster_image) : nullptr;
        lv_obj_t* col = reuse_raster ? lv_obj_get_parent(slot) : lv_obj_create(st.reader.content);
        if (!reuse_raster) {
            lv_obj_remove_style_all(col);
        }
        lv_obj_set_size(col, col_w, ink_h);
        lv_obj_set_pos(col, col_x, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(col);

        if (!reuse_raster) {
            slot = lv_obj_create(col);
            lv_obj_remove_style_all(slot);
        }
        lv_obj_set_size(slot, col_w, ink_h);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        if (reuse_raster) {
            heap_img->BindDsc();
            lv_image_set_src(st.reader.text_raster_image, &heap_img->dsc);
            lv_obj_invalidate(st.reader.text_raster_image);
        } else {
            AttachPageImageToSlot(slot, heap_img, false);
            st.reader.text_raster_image = lv_obj_get_child(slot, 0);
            lv_obj_add_event_cb(
                st.reader.text_raster_image,
                [](lv_event_t* event) {
                    auto& image = Book_State().reader.text_raster_image;
                    if (image == lv_event_get_target(event)) {
                        image = nullptr;
                    }
                },
                LV_EVENT_DELETE, nullptr);
        }
        EpdSetContentRole(st.reader.text_raster_image, BookReaderPrefsAntialias() || background == 3
                                                           ? EpdContentRole::ReaderAntialias
                                                           : EpdContentRole::Monochrome);

        size_t image_index = 0;
        for (const auto& item : page->items) {
            if (item.kind != ContentKind::kText) {
                attach_image(col, item, col_w, prepared->image_tops[image_index++]);
            }
        }
        StartPageImageWorker(prepared->direction);
        UpdateReadFooter();
        FinishReaderFirstPaintGate();
        ResetPointerLongPress();
        if (settings_overlay) {
            SetReadStatusVisible(true);
            SetReadSettingsSheetVisible(true);
        }
        ESP_LOGD(TAG, "RenderReaderPage ok %dx%d cost=%lldms", col_w, ink_h,
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000));
        return;
    }

    // LVGL label/插图路径
    if (reuse_raster && st.reader.text_raster_image) {
        lv_obj_clean(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    const lv_coord_t col_w =
        page_images ? max_w : PageTextColumnWidth(*page, book_font, max_w, st.reader.session->Style());
    const lv_coord_t col_x = content_area.x1 + (max_w - col_w) / 2;

    lv_obj_t* col = lv_obj_create(st.reader.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, col_w, page_images ? content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_set_pos(col, col_x, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(col);

    const auto lines =
        MakeTextLineLayout(book_font, content_h, spacing_percent, false, st.reader.session->Style().line_gap);
    lv_obj_set_style_text_letter_space(col, st.reader.session->Style().letter_gap, 0);
    int row_index = 0;
    bool prev_chapter_title = false;
    for (const auto& item : page->items) {
        if (item.kind == ContentKind::kText) {
            if (item.text.empty()) {
                continue;
            }
            const lv_coord_t gap = (item.para_gap_before && prev_chapter_title)
                                       ? static_cast<lv_coord_t>(para_gap + kReaderChapterTitleAfterGap)
                                       : para_gap;
            CreatePageTextLabel(
                col, item, col_w, book_font, lines, row_index, gap,
                [](lv_obj_t* label) { AttachReaderUnderline(label, BookReaderPrefsUnderlineMode()); },
                st.reader.session->Style());
            prev_chapter_title = item.chapter_title;
        } else {
            prev_chapter_title = false;
            attach_image(col, item, col_w, -1);
        }
    }

    StartPageImageWorker(prepared->direction);

    UpdateReadFooter();
    FinishReaderFirstPaintGate();
    // 重绘阻塞期间 tick 仍走：复位长按计时，避免恢复后一次判定就 LONG_PRESSED
    ResetPointerLongPress();
    if (settings_overlay) {
        SetReadStatusVisible(true);
        SetReadSettingsSheetVisible(true);
    }
}

void ShowReaderPageLoading() {
    auto& st = Book_State();
    if (!st.reader.content || st.reader.standby_paused) {
        return;
    }
    CancelPageImageLoad();
    if (st.reader.background_layer && lv_obj_is_valid(st.reader.background_layer)) {
        lv_obj_delete(st.reader.background_layer);
    }
    st.reader.background_layer = nullptr;
    st.reader.background_image.reset();
    if (st.reader.footer_host) {
        lv_obj_set_style_bg_opa(st.reader.footer_host, LV_OPA_COVER, 0);
    }
    SyncReaderAaForImmersion(false);
    EpdSetContentRole(st.reader.content, EpdContentRole::Monochrome);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    Book_ShowMessage(st.reader.content, LangString_BOOK_LOADING);
}
