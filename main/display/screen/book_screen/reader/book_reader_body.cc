#include "assets/lang_reader.h"
#include "epd_content_policy.h"
#pragma GCC optimize("O1")

#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_page_image.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_vk.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include <memory>
#include <utility>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "display_orient.h"
#include "lv_adapter_display.h"
#include "reader/reader.h"
#include "reader/reader_page_render.h"
#include "reader/page_text_raster.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_timer.h>

namespace {
bool s_reader_first_paint = false;

void FinishReaderFirstPaintGate() {
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
}  // namespace

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

    lv_area_t coords;
    lv_obj_get_content_coords(label, &coords);
    const lv_coord_t line_h = font->line_height;
    // 相对 LVGL 默认下划线再下移 4px
    const lv_coord_t y_in_line =
        static_cast<lv_coord_t>(line_h - font->base_line - font->underline_position + 4);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_black();
    // 相对字体 underline_thickness 再加粗 1px（实线/虚线同宽）
    const lv_coord_t base_w = font->underline_thickness > 0 ? font->underline_thickness : 1;
    dsc.width = static_cast<lv_coord_t>(base_w + 1);
    dsc.opa = LV_OPA_COVER;
    if (mode == kBookReaderUnderlineDashed) {
        dsc.dash_width = 6;
        dsc.dash_gap = 4;
    }

    const char* txt = lv_label_get_text(label);
    lv_point_t sz = {};
    if (txt != nullptr && txt[0] != '\0') {
        lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }

    const lv_coord_t content_h = lv_area_get_height(&coords);
    const int lines = content_h > 0 ? static_cast<int>((content_h + line_h - 1) / line_h) : 1;
    for (int i = 0; i < lines; ++i) {
        const lv_coord_t y = coords.y1 + i * line_h + y_in_line;
        dsc.p1.x = coords.x1;
        dsc.p1.y = y;
        lv_coord_t x2 = coords.x2;
        if (lines == 1 && sz.x > 0) {
            x2 = coords.x1 + sz.x - 1;
            if (x2 > coords.x2) {
                x2 = coords.x2;
            }
        }
        if (x2 < dsc.p1.x) {
            continue;
        }
        dsc.p2.x = x2;
        dsc.p2.y = y;
        lv_draw_line(layer, &dsc);
    }
}

void AttachReaderUnderline(lv_obj_t* label, int mode) {
    if (label == nullptr || mode == kBookReaderUnderlineOff) {
        return;
    }
    lv_obj_set_user_data(label, reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
    lv_obj_add_event_cb(label, OnReaderUnderlineDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
}

static uint8_t ReadingBackgroundPixel(int mode, int x, int y, int left, int width, int height) {
    if (x < left || x >= left + width || y < 0 || y >= height) return 255;
    x -= left;
    constexpr int pitch = 32; // Shared grid cell and dot spacing in display pixels.
    if (mode == 2) {
        const int dot_x = x % pitch, dot_y = y % pitch;
        // I1 output thresholds gray to black/white. A 4x4 checker keeps the dot
        // footprint while reducing ink coverage to 50% for a lighter appearance.
        return (dot_x < 4 && dot_y < 4 && ((dot_x + dot_y) % 2 == 0)) ? 0 : 255;
    }
    if (mode == 1) {
        const bool vertical = x % pitch == 0 || x == width - 1;
        const bool horizontal = y % pitch == 0 || y == height - 1;
        // Close the grid at the viewport edges even when its size is not a multiple of 32.
        const bool corner = (x == 0 || x == width - 1) && (y == 0 || y == height - 1);
        return ((vertical && y % 3 == 0) || (horizontal && x % 3 == 0) || corner) ? 0 : 255;
    }
    return 255;
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
    if (st.reader.footer_host) {
        lv_obj_set_style_bg_opa(st.reader.footer_host, LV_OPA_COVER, 0);
    }
    SyncReaderAaForImmersion(false);
    EpdSetContentRole(st.reader.content, EpdContentRole::Monochrome);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_LOADING);
}

void RenderReaderPage() {
    if (Book_State().reader.standby_paused) return;
    // Start a fresh interval after every render, including manual turns and layout changes.
    struct AutoTurnResetOnExit { ~AutoTurnResetOnExit() {
        if (ReaderAutoTurnEnabled()) ResetReaderAutoTurnDeadline();
    } } auto_turn_reset;
    ApplyReaderPageDelta();
    auto& st = Book_State();
    if (st.reader.content == nullptr || !st.reader.session) {
        s_reader_first_paint = false;
        return;
    }
    // Loading/errors/TOC stay binary; opt into text gray only when a page exists.
    EpdSetContentRole(st.reader.content,EpdContentRole::Monochrome);
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
    SyncReaderAaForImmersion(!settings_overlay);
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
    // 分页视口与当前几何不一致：重排（宽或高任一过期都会让末行挤进底栏 / 行宽偏短）
    if (!page_images) {
        if (st.reader.viewport_w > 40 && st.reader.viewport_h > 40 && vw > 40 && vh > 40 &&
            viewport_changed && !st.reader.opening.load() &&
            !st.reader.layout_busy.load() && !st.reader.session->LiveRelayoutPending()) {
            ESP_LOGW(TAG, "viewport mismatch session=%dx%d ui=%dx%d → relayout",
                     static_cast<int>(vw), static_cast<int>(vh),
                     static_cast<int>(st.reader.viewport_w), static_cast<int>(st.reader.viewport_h));
            ApplyReaderLayoutLive(false);
            return;
        }
    }

    // 视口尚未生效或章节仍在重排时，不能把旧页按新方向重新绘制。
    if (st.reader.layout_again || viewport_changed ||
        (st.reader.session->Info().format != reader::BookFormat::kTxt &&
         st.reader.session->LiveRelayoutPending())) {
        if (!ReaderWorkersBusy() && st.reader.session->Error() != reader::BookReadError::kNone) {
            Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
        } else {
            ShowReaderPageLoading();
        }
        return;
    }

    CancelPageImageLoad();
    const reader::Page* page = st.reader.session->CurrentPageData();
    const bool reuse_raster = page && !page_images && BookReaderPrefsBackground() == 0 &&
        reader::PageIsTextOnly(*page) && st.reader.text_raster_image &&
        lv_obj_is_valid(st.reader.text_raster_image) && lv_obj_get_child_count(st.reader.content) == 1;
    if (!reuse_raster) {
        lv_obj_clean(st.reader.content);
        Book_DisableScroll(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    if (page == nullptr) {
        FinishReaderFirstPaintGate();
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
        if (settings_overlay) {
            SetReadStatusVisible(true);
            SetReadSettingsSheetVisible(true);
        }
        return;
    }

    EpdSetContentRole(st.reader.content,!page_images && BookReaderPrefsAntialias()
        ? EpdContentRole::ReaderAntialias : EpdContentRole::Monochrome);
    PrewarmPageFont(page);

    const lv_font_t* book_font = BookFont();
    const lv_coord_t max_w = page_images ? LV_HOR_RES : ReadContentWidth();
    const lv_coord_t content_h = st.reader.viewport_h;
    const int background = page_images ? 0 : BookReaderPrefsBackground();
    const int background_width = ReadContentWidth();
    const int background_left = st.reader.session->VerticalWriting()
        ? BookReaderPrefsMarginLeft() + kReaderVerticalLeftInset : (LV_HOR_RES - background_width) / 2;
    const int background_height = std::max<int>(0, std::min<int>(content_h,
        LV_VER_RES - BookReaderPrefsMarginTop() - BookReaderPrefsMarginBottom()));
    if (st.reader.background_layer && lv_obj_is_valid(st.reader.background_layer)) lv_obj_delete(st.reader.background_layer);
    st.reader.background_layer = nullptr;
    if (st.reader.footer_host) lv_obj_set_style_bg_opa(st.reader.footer_host, background ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    if (background && background_height > 0) {
        auto* image = new reader::RasterImage();
        image->width = LV_HOR_RES; image->height = background_height;
        image->pixels.resize(static_cast<size_t>(image->width) * image->height);
        for (int y=0; y<image->height; ++y) for (int x=0; x<image->width; ++x)
            image->pixels[y*image->width+x] = ReadingBackgroundPixel(background,x,y,background_left,background_width,background_height);
        image->BindDsc();
        auto* slot = lv_obj_create(st.reader.read_scr);
        lv_obj_remove_style_all(slot);
        lv_obj_set_size(slot, image->width, image->height);
        lv_obj_set_pos(slot, 0, BookReaderPrefsMarginTop());
        st.reader.background_layer = slot;
        lv_obj_move_background(slot);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(slot);
        AttachPageImageToSlot(slot, image, false);
    }
    const lv_coord_t line_gap = st.reader.session->LineGap();
    const lv_coord_t para_gap = st.reader.session->ParaGap();

    // 纯文本页：预渲 L8 → 单张 lv_image（PARTIAL 上屏）
    if (!page_images && book_font != nullptr && reader::PageIsTextOnly(*page) && content_h > 40 &&
        max_w > 40) {
        const int64_t t0 = esp_timer_get_time();
        const bool vertical = st.reader.session->VerticalWriting();
        lv_coord_t block_w = 0;
        for (const auto& item : page->items) {
            if (item.kind != reader::ContentKind::kText || item.text.empty()) {
                continue;
            }
            lv_point_t sz = {};
            lv_text_get_size(&sz, item.text.c_str(), book_font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            lv_coord_t w = static_cast<lv_coord_t>(sz.x);
            if (item.para_indent) {
                w = static_cast<lv_coord_t>(w + reader::ParaIndentPadPx(book_font));
            }
            if (w > block_w) {
                block_w = w;
            }
        }
        lv_coord_t col_w = max_w;
        if (!vertical && block_w > 40 && block_w < max_w) {
            col_w = block_w;
        }
        // 竖排额外左留白与分页宽度同步，保留页边距设置。
        const lv_coord_t col_x = vertical ? BookReaderPrefsMarginLeft() + kReaderVerticalLeftInset
            : static_cast<lv_coord_t>((LV_HOR_RES - col_w) / 2);
        int ink_h = vertical ? content_h : reader::EstimatePageTextHeight(*page, book_font, line_gap, para_gap, content_h);
        if (ink_h < 8) {
            ink_h = 8;
        }
        if (ink_h > content_h) {
            ink_h = content_h;
        }

        auto* heap_img = reuse_raster
            ? static_cast<reader::RasterImage*>(lv_obj_get_user_data(st.reader.text_raster_image))
            : new reader::RasterImage();
        if (reuse_raster) {
            lv_image_cache_drop(&heap_img->dsc);
            lv_image_header_cache_drop(&heap_img->dsc);
        }
        const bool raster_ok = vertical
            ? reader::RasterPageVerticalTextToL8(*page, book_font, line_gap, para_gap, col_w, ink_h,
                                                BookReaderPrefsUnderlineMode(), *heap_img, BookReaderPrefsAntialias() != 0)
            : reader::RasterPageTextToL8(*page, book_font, line_gap, para_gap, col_w, ink_h,
                                        BookReaderPrefsUnderlineMode(), *heap_img, BookReaderPrefsAntialias() != 0);
        if (raster_ok &&
            !heap_img->empty()) {
            if (background) {
                for (int y=0; y<heap_img->height; ++y) for (int x=0; x<heap_img->width; ++x) {
                    auto& pixel = heap_img->pixels[y*heap_img->width+x];
                    if (pixel == 255) pixel = ReadingBackgroundPixel(background, col_x+x, y, background_left, background_width, background_height);
                }
            }
            lv_obj_t* slot = reuse_raster ? lv_obj_get_parent(st.reader.text_raster_image) : nullptr;
            lv_obj_t* col = reuse_raster ? lv_obj_get_parent(slot) : lv_obj_create(st.reader.content);
            if (!reuse_raster) lv_obj_remove_style_all(col);
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
                lv_obj_add_event_cb(st.reader.text_raster_image, [](lv_event_t* event) {
                    auto& image = Book_State().reader.text_raster_image;
                    if (image == lv_event_get_target(event)) image = nullptr;
                }, LV_EVENT_DELETE, nullptr);
            }
            EpdSetContentRole(st.reader.text_raster_image,BookReaderPrefsAntialias()
                ? EpdContentRole::ReaderAntialias : EpdContentRole::Monochrome);
            heap_img = nullptr;

            UpdateReadFooter();
            FinishReaderFirstPaintGate();
            ResetPointerLongPress();
            if (settings_overlay) {
                SetReadStatusVisible(true);
                SetReadSettingsSheetVisible(true);
            }
            ESP_LOGI(TAG, "RenderReaderPage ok %dx%d cost=%lldms", col_w, ink_h,
                     static_cast<long long>((esp_timer_get_time() - t0) / 1000));
            return;
        }
        if (reuse_raster) {
            lv_obj_clean(st.reader.content); // Image DELETE owns heap_img.
            AttachReadBodyInput(st.reader.content);
        } else delete heap_img;
        ESP_LOGW(TAG, "RenderReaderPage raster fail → label path");
    }

    // LVGL label/插图路径
    if (reuse_raster && st.reader.text_raster_image) {
        lv_obj_clean(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    // 按本页实际行宽收栏：行未撑满视口时居中整栏，避免满宽 label 左齐造成右空
    lv_coord_t block_w = 0;
    if (!page_images && book_font != nullptr) {
        for (const auto& item : page->items) {
            if (item.kind != reader::ContentKind::kText || item.text.empty()) {
                continue;
            }
            lv_point_t sz = {};
            lv_text_get_size(&sz, item.text.c_str(), book_font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            lv_coord_t w = static_cast<lv_coord_t>(sz.x);
            if (item.para_indent) {
                w = static_cast<lv_coord_t>(w + reader::ParaIndentPadPx(book_font));
            }
            if (w > block_w) {
                block_w = w;
            }
        }
    }
    lv_coord_t col_w = max_w;
    if (!page_images && block_w > 40 && block_w < max_w) {
        col_w = block_w;
    }
    const lv_coord_t col_x =
        page_images ? 0 : static_cast<lv_coord_t>((LV_HOR_RES - col_w) / 2);

    lv_obj_t* col = lv_obj_create(st.reader.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, col_w, page_images ? content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, page_images ? 0 : line_gap, 0);
    lv_obj_set_pos(col, col_x, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(col);

    bool prev_chapter_title = false;
    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            if (item.text.empty()) {
                continue;
            }
            const lv_coord_t gap =
                (item.para_gap_before && prev_chapter_title)
                    ? static_cast<lv_coord_t>(para_gap + reader::kReaderChapterTitleAfterGap)
                    : para_gap;
            lv_obj_t* label =
                reader::CreatePageTextLabel(col, item, col_w, book_font, line_gap, gap);
            AttachReaderUnderline(label, BookReaderPrefsUnderlineMode());
            prev_chapter_title = item.chapter_title;
        } else {
            prev_chapter_title = false;
            const lv_coord_t max_h =
                page_images ? content_h : (content_h * 3 / 4);
            const int img_h = max_h > 40 ? max_h : 200;
            lv_obj_t* slot = lv_obj_create(col);
            lv_obj_remove_style_all(slot);
            lv_obj_set_width(slot, col_w);
            lv_obj_set_height(slot, page_images ? content_h : LV_SIZE_CONTENT);
            lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);

            reader::RasterImage cached;
            if (st.reader.session->TryCopyCachedPageImage(item.image_href, cached) && !cached.empty()) {
                auto* heap_img = new reader::RasterImage(std::move(cached));
                AttachPageImageToSlot(slot, heap_img, page_images);
            } else {
                lv_obj_t* label = lv_label_create(slot);
                lv_label_set_text(label, Lang::Strings::BOOK_IMAGE_PLACEHOLDER);
                lv_obj_set_style_text_font(label, book_font, 0);
                lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
                BookUiState::PageImagePending pending;
                pending.href = item.image_href;
                pending.max_w = col_w;
                pending.max_h = img_h;
                pending.slot = slot;
                st.reader.page_image_pending.push_back(std::move(pending));
            }
        }
    }

    StartPageImageWorker();

    UpdateReadFooter();
    FinishReaderFirstPaintGate();
    // 重绘阻塞期间 tick 仍走：复位长按计时，避免恢复后一次判定就 LONG_PRESSED
    ResetPointerLongPress();
    if (settings_overlay) {
        SetReadStatusVisible(true);
        SetReadSettingsSheetVisible(true);
    }
}

lv_obj_t* CreateReaderScreen(const reader::BookInfo& info) {
    auto& st = Book_State();
    // 硬护栏：layout/open worker 仍用旧 session 时禁止 make_unique 拆掉（UAF）
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("CreateReaderScreen");
        return BookScreen::Create();
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
    }
    s_reader_first_paint = true;
    StopReaderAutoOrient();
    DisplayUiSetOrient(ReaderDisplayOrient());
    // 仅打开正文时加载 SD 字体；界面控件用固件字
    EnsureBookFont();
    CancelReadFontScan();
    // 换新 session 前停旧 timer/墙钟，避免回调 UAF 或漏写本段秒数
    EndReadingTimeTracking();
    st.reader.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.reader.session);
    st.reader.opening_format = info.format;

    st.reader.background_layer = nullptr;
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

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
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sheet, kSheetBorderW, 0);
    lv_obj_set_style_border_color(sheet, lv_color_black(), 0);
    lv_obj_set_style_radius(sheet, kSheetRadius, 0);
    lv_obj_set_style_pad_all(sheet, kSheetInnerPad, 0);
    lv_obj_set_style_pad_row(sheet, kSheetGap, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    Book_DisableScroll(sheet);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    // 点在卡片空白处不穿透到正文（避免误收）
    lv_obj_add_event_cb(
        sheet, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    // 底栏白底槽：正文不侵入；相对槽底上留与面板 BOTTOM_INSET 一致
    lv_obj_t* foot_host = lv_obj_create(scr);
    lv_obj_remove_style_all(foot_host);
    lv_obj_set_size(foot_host, LV_HOR_RES, ReaderFooterHeight());
    lv_obj_align(foot_host, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(foot_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(foot_host, 0, 0);
    Book_DisableScroll(foot_host);
    lv_obj_clear_flag(foot_host, LV_OBJ_FLAG_CLICKABLE);
    st.reader.footer_host = foot_host;

    lv_obj_t* footer = lv_label_create(foot_host);
    lv_obj_set_width(footer, LV_HOR_RES - UiSx(16));
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(footer, "");
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CONTENT_BOTTOM_INSET);
    Book_DisableScroll(footer);
    st.reader.page_label = footer;

    ShowOpenProgress(0);
    st.reader.read_scr = scr;
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_target(e) == Book_State().reader.read_scr) StopReaderAutoOrient();
    }, LV_EVENT_DELETE, nullptr);
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

