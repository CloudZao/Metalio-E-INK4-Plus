#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>
#include <vector>
#include <strings.h>
#include <dirent.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ui/common/widgets/storage_error_text.h"
#include "ui/common/widgets/fixed_footer.h"
#include "assets/lang_screen.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "assets/lang_reader.h"
#include "boards/common/bus/flash_access.h"
#include "business/reader/txt_chapter.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/screen_draw.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/cover/book_cover_cell.h"
#include "ui/apps/book/cover/book_cover_style.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "ui/apps/book/reader/book_auto_turn.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_layout_worker.h"
#include "ui/apps/book/reader/book_reader_body.h"
#include "ui/apps/book/reader/book_reader_orient.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_bookmark_art.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_sheet_icons.h"
#include "ui/apps/book/settings/book_tap_ui.h"
#include "business/reader/book_tap_zones.h"
#include "ui/apps/book/settings/book_ttf_actions.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/common/widgets/screen_layout.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/drawing/ui_theme.h"
#include "assets/lang_config.h"
#include "business/reader/book_reader_prefs.h"
#include "business/reader/book_bookmark_jobs.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_scrollbar.h"

void BuildReadingBottomSheet();
static void OnSheetFooterLayout(lv_event_t* event);

static constexpr int kDurationTab = 4;
static constexpr int kReadingCardSideInset = 4;    // 主卡片左右内收的物理像素
static constexpr int kReadingCardBorderWidth = 2;  // 顶部及导航横向分隔线宽度

struct BookSettingsSheetSheetBinding {
    lv_obj_t* button;
    lv_event_cb_t event;
    int value;
    bool mask;
};

static std::vector<BookSettingsSheetSheetBinding> s_sheet_bindings;
static int s_toc_count = 0;
static constexpr int kSheetProgressBase = 10000;  // 进度跳转编码起点，与目录索引区分
static int s_toc_items = 0;                       // 包含各页首页入口的显示项总数
static bool s_toc_home = false;                   // 目录页是否为首页入口预留一行
static bool s_toc_building = false;
static lv_obj_t* s_live_body = nullptr;
static lv_obj_t *s_bookmark_button = nullptr, *s_bookmark_icon = nullptr, *s_duration_value = nullptr,
                *s_progress_value = nullptr, *s_progress_slider = nullptr, *s_chapter_prev = nullptr,
                *s_chapter_next = nullptr;
static int s_live_tab = -1, s_live_width = 0, s_live_height = 0;
static int s_live_settings_page = -1;
static uint32_t s_live_font_revision = 0; // 现有字体控件对应的目录内容代次

static lv_coord_t SheetNavHeight() {
    const int height = LV_HOR_RES > LV_VER_RES
                           ? std::min<int>(UiSy(74), std::max<int>(UiFont(UiFontRole::TitleEmphasis)->line_height,
                                                                   Book_ItemFont()->line_height) +
                                                         54)
                           : UiSy(74);
    const int content_height =
        std::max<int>(UiFont(UiFontRole::TitleEmphasis)->line_height, Book_ItemFont()->line_height) +
        std::max({SheetFontIcon()->header.h, SheetTocIcon()->header.h, SheetBookmarkNavIcon()->header.h,
                  SheetSettingsIcon()->header.h}) +
        UiSy(32);
    return std::min<int>(std::max(height, content_height), LV_VER_RES / 6);
}

static int ReadingSheetTabHeaderHeight() {
    const int tab = Book_State().settings.reader_tab;
    if (tab == kDurationTab) {
        return 0;
    }
    if (tab == 2 && LV_HOR_RES > LV_VER_RES) {
        return std::min<int>(UiSy(56), UiFont(UiFontRole::TitleEmphasis)->line_height + 12);
    }
    return ReadingSheetHeaderHeight();
}

static int ReadingSheetBodyPadding() {
    const int tab = Book_State().settings.reader_tab;
    return tab == 0 || (tab == 2 && LV_HOR_RES > LV_VER_RES) ? 0 : UiSy(4);
}

static int SettingsRowHeight() {
    return std::max<int>(UiSy(60), ScreenTextHeight(Book_ItemFont()) + UiSy(20) + 4);
}

static int SettingsCardHeight() {
    return std::max<int>(UiSy(82), ScreenTextHeight(Book_ItemFont(), 2) + UiSy(21) + 4);
}

static int SettingsGridContentHeight() {
    // 阅读设置首页 3×3 的内容高度，触摸分区沿用同一块区域。
    return 3 * SettingsCardHeight() + 2 * UiSy(9);
}

static int SettingsPreviewButtonHeight() {
    return std::max<int>(UiSy(150), ScreenTextHeight(Book_ItemFont(), 4) + UiSy(12) + 4);
}

static int ReadingSummaryHeight() {
    return std::max<int>(UiSy(80), 2 * Book_ItemFont()->line_height + UiSy(22 + 5 + 4));
}

static int ReadingProgressControlsHeight() {
    return std::max<int>(UiSy(44), Book_ItemFont()->line_height + UiSy(16));
}

static lv_obj_t* SheetLabel(lv_obj_t* parent, const char* text, const lv_font_t* font = nullptr) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font ? font : Book_ItemFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t* SheetBox(lv_obj_t* parent, int w, int h) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    Book_DisableScroll(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static lv_obj_t* SheetButton(lv_obj_t* parent, const char* text, int w, int h, lv_event_cb_t cb, intptr_t value = 0,
                             bool active = false, lv_coord_t radius = LV_RADIUS_CIRCLE, bool align_left = false) {
    auto* btn = SheetBox(parent, w, h);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(btn, active ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(value));
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
    auto* label = SheetLabel(btn, text);
    const int inset = align_left ? UiSx(12) : 6;
    ScreenFitLabel(label, std::max<int>(1, w - static_cast<int>(inset) * 2), h - 6,
                   active ? UiFont(UiFontRole::Emphasis) : Book_ItemFont());
    lv_obj_set_style_text_color(label, active ? UiColorBackground() : UiColorForeground(), 0);
    if (align_left) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, inset, 0);
    } else {
        lv_obj_center(label);
    }
    return btn;
}

static void SheetTabEvent(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    SelectReadingSheetTab(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

static void SheetClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto& st = Book_State();
    if (st.ttf.ttf_multi) {
        OnTtfMultiCancel(e);
    } else if (st.settings.font_multi) {
        ExitFontMultiMode(true);
    } else if (st.settings.bookmark_multi) {
        ResetBookmarkSelection();
        RequestSettingsSheetPaint();
    } else {
        BookLvAsync(AsyncHideReadChrome);
    }
}

static void SheetScrollBegin(lv_event_t* e) {
    auto& settings = Book_State().settings;
    if (lv_event_get_target(e) != settings.reader_sheet_scroll) {
        return;
    }
    settings.reader_scroll_last = lv_obj_get_scroll_y(settings.reader_sheet_scroll);
    settings.reader_sheet_scrolling = true;
    EpdSetUiScrolling(true);
}

static void SheetScrollMove(lv_event_t* e) {
    auto& settings = Book_State().settings;
    auto* list = static_cast<lv_obj_t*>(lv_event_get_target(e));
    static bool adjusting = false;
    if (adjusting || !settings.reader_sheet_scrolling || list != settings.reader_sheet_scroll ||
        !TouchUiFingerIsDown()) {
        return;
    }
    // 仅加速当前输入设备实际拖动的列表，恢复位置和按钮翻页不参与加速。
    auto* indev = lv_indev_active();
    if (!indev || lv_indev_get_scroll_obj(indev) != list) {
        return;
    }
    const int current = lv_obj_get_scroll_y(list);
    const int delta = current - settings.reader_scroll_last;
    if (delta) {
        const int maximum = std::max<int>(0, current + lv_obj_get_scroll_bottom(list));
        const int target = std::max<int>(0, std::min<int>(maximum, current + delta * 2));
        adjusting = true;
        lv_obj_scroll_to_y(list, target, LV_ANIM_OFF);
        adjusting = false;
    }
    settings.reader_scroll_last = lv_obj_get_scroll_y(list);
}

static void SheetScrollEnd(lv_event_t* e) {
    auto& settings = Book_State().settings;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (target == settings.reader_sheet_scroll && tab >= 0 && tab < 5) {
        settings.reader_scroll[tab] = lv_obj_get_scroll_y(target);
    }
    if (target == settings.reader_sheet_scroll && settings.reader_sheet_scrolling) {
        settings.reader_sheet_scrolling = false;
        EpdSetUiScrolling(false);
        const auto& session = Book_State().reader.session;
        if (tab == 0 && session &&
            (s_toc_count != session->TocCount() || s_toc_building != session->LiveRelayoutPending() ||
             s_toc_home != session->TocStartsAfterFirstPage())) {
            RequestSettingsSheetPaint();
        }
    }
}

static bool SheetNavigationReady() {
    const auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen() || st.reader.prefs_reopen_pending ||
        st.reader.layout_again || st.reader.opening.load()) {
        return false;
    }
    return !st.reader.deferred_cleanup && !st.reader.standby_paused;
}

static void SheetJumpAsync(void* data) {
    if (!SheetNavigationReady()) {
        return;
    }
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(data));
    const bool progress = encoded >= kSheetProgressBase;
    const auto kind = progress ? BookSession::NavigationKind::kProgress : BookSession::NavigationKind::kToc;
    const int target = progress ? encoded - kSheetProgressBase : encoded;
    const bool ok = RequestReaderNavigation(kind, target);
    if (ok) {
        HideReadChrome();
        RequestRenderReaderPage();
    }
}

static void SheetTocJump(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookLvAsync(SheetJumpAsync, lv_event_get_user_data(e));
}

static void SheetChapterAsync(void* data) {
    auto& st = Book_State();
    if (!SheetNavigationReady() || st.reader.read_chrome != BookUiState::ReadChrome::kSettings) {
        return;
    }
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(data));
    const auto kind = st.reader.session->IsPageImagesMode() ? BookSession::NavigationKind::kDelta
                                                            : BookSession::NavigationKind::kChapterDelta;
    if (RequestReaderNavigation(kind, delta)) {
        RequestRenderReaderPage();
        RequestSettingsSheetPaint();
    }
}

static void SheetChapter(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookLvAsync(SheetChapterAsync, lv_event_get_user_data(e));
}

static void SheetSeek(lv_event_t* e) {
    const int value = lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    BookLvAsync(SheetJumpAsync, reinterpret_cast<void*>(static_cast<intptr_t>(kSheetProgressBase + value)));
}

static lv_obj_t* SheetSlider(lv_obj_t* parent, int value, int max, int w) {
    auto* slider = lv_slider_create(parent);
    lv_obj_set_width(slider, w);
    ScreenStyleProgressSlider(slider);
    lv_slider_set_range(slider, 0, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    return slider;
}

static void ConfigureSheetScroll(lv_obj_t* list) {
    auto& settings = Book_State().settings;
    settings.reader_sheet_scroll = list;
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, UiColorForeground(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, UiSx(5), LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(list, SheetScrollMove, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(list, SheetScrollBegin, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(list, SheetScrollEnd, LV_EVENT_SCROLL_END,
                        reinterpret_cast<void*>(static_cast<intptr_t>(settings.reader_tab)));
}

static int SettingsColumn(lv_obj_t* parent, std::initializer_list<const char*> captions) {
    const auto* font = Book_ItemFont();
    int text = static_cast<int>(UiSx(72));
    if (font) {
        for (const char* caption : captions) {
            if (caption && *caption) {
                text = std::max<int>(
                    text, static_cast<int>(ScreenMeasureTextWidth(font, caption)) + static_cast<int>(UiSx(8)));
            }
        }
    }
    lv_obj_update_layout(parent);
    const int row = std::max<int>(1, static_cast<int>(lv_obj_get_content_width(parent)));
    const int cap = std::max<int>(static_cast<int>(UiSx(88)), row - static_cast<int>(UiSx(180)));
    return std::min<int>(text, cap);
}

static void PlaceSettingsCaption(lv_obj_t* label, int column) {
    lv_obj_set_style_text_font(label, Book_ItemFont(), 0);
    lv_obj_set_width(label, std::max<int>(1, column - static_cast<int>(UiSx(8))));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
}

static lv_obj_t* SettingsRow(lv_obj_t* parent, const char* title, int count, const char* const* names,
                             lv_event_cb_t event, const int* values, int selected, bool mask = false, int slots = 0,
                             int column = 0, int row_height = 0) {
    const int w = ScreenContentSize(parent).width;
    const int h = std::max<int>(row_height, SettingsRowHeight());
    const int start = column > 0 ? column : SettingsColumn(parent, {title});
    const int gap = UiSx(8);
    int minimum_width = Book_ItemFont()->line_height * 2;
    for (int i = 0; i < count; ++i) {
        minimum_width = std::max<int>(minimum_width, ScreenMeasureTextWidth(Book_ItemFont(), names[i]) + UiSx(16));
    }
    const int wanted = slots > count ? slots : count;
    const int columns = UiLayoutCount(w - start, minimum_width, gap, wanted);
    const int rows = (count + columns - 1) / columns;
    auto* row = SheetBox(parent, w, h * rows);
    if (title && *title) {
        auto* label = SheetLabel(row, title);
        PlaceSettingsCaption(label, start);
    }
    const int bw = std::max<int>(1, (static_cast<int>(w) - start - static_cast<int>(gap) * (columns - 1)) / columns);
    for (int i = 0; i < count; ++i) {
        const bool on = mask ? (selected & values[i]) != 0 : selected == values[i];
        const int button_h = h - UiSy(12);
        auto* btn = SheetButton(row, names[i], bw, button_h, event, values[i], on, UiSx(7));
        lv_obj_set_pos(btn, start + i % columns * (bw + gap), i / columns * h + (h - button_h) / 2);
        s_sheet_bindings.push_back({btn, event, values[i], mask});
    }
    return row;
}

enum BookSettingsSheetSettingsPage {
    kSettingsHome,
    kSettingsRotation,
    kSettingsFooter,
    kSettingsTapZones,
    kSettingsBackground,
    kSettingsWriting,
    kSettingsAntialias,
    kSettingsSpacing,
    kSettingsUnderline,
    kSettingsTurning
};

static constexpr int kAdjustFont = 100;
static constexpr int kAdjustAutoTurn = 101;
static constexpr int kAdjustShake = 102;
static BookTextOption s_margin_option = BookTextOption::MarginTop;  // 当前选中的阅读边距

struct BookSettingsSheetNumericRow {
    int option = 0, value = 0, original = 0, minimum = 0, maximum = 0, step = 1;  // 手势内的参数快照
    lv_obj_t* label = nullptr;                                                    // 数值显示，重建前清理
    bool active = false;                                                          // 按住期间不重建控件
    lv_obj_t *unit = nullptr, *decrease = nullptr, *increase = nullptr;           // 数值单位和调节按钮
    bool adjustable = true;                                                       // EF 字体不允许改变字号
};

static std::vector<BookSettingsSheetNumericRow> s_numeric_rows;
static bool s_numeric_refresh_pending = false; // 数值调节结束后补做同页后台刷新

static bool NumericAdjustmentActive() {
    return std::any_of(s_numeric_rows.begin(), s_numeric_rows.end(), [](const auto& row) { return row.active; });
}

static void UpdateNumericValue(BookSettingsSheetNumericRow& row) {
    char text[24];
    std::snprintf(text, sizeof(text), "%d", row.value);
    const char* previous = row.label ? lv_label_get_text(row.label) : nullptr;
    const bool changed = !previous || std::strcmp(previous, text) != 0;
    if (!changed) {
        return;
    }
    lv_label_set_text(row.label, text);
    EpdRequestBlackEqualization();
    if (!row.unit) {
        return;
    }
    auto* box = lv_obj_get_parent(row.label);
    const auto* number_font = Book_ItemFont();
    const auto* unit_font = Book_ItemFont();
    lv_obj_set_style_text_font(row.label, number_font, 0);
    lv_obj_set_style_text_font(row.unit, unit_font, 0);
    lv_obj_set_size(row.label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_size(row.unit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_update_layout(box);
    const int gap = UiSx(2), number_width = lv_obj_get_width(row.label);
    const int left = (lv_obj_get_content_width(box) - number_width - lv_obj_get_width(row.unit) - gap) / 2;
    lv_obj_align(row.label, LV_ALIGN_LEFT_MID, left, 0);
    const int baseline =
        (number_font->line_height - unit_font->line_height) / 2 - number_font->base_line + unit_font->base_line;
    lv_obj_align(row.unit, LV_ALIGN_LEFT_MID, left + number_width + gap, baseline);
}

static void UpdateNumericBounds(BookSettingsSheetNumericRow& row) {
    if (!row.decrease || !row.increase) {
        return;
    }
    for (auto* button : {row.decrease, row.increase}) {
        const bool enabled =
            row.adjustable && (button == row.decrease ? row.value > row.minimum : row.value < row.maximum);
        SetSheetBtnEnabled(button, enabled);
        if (enabled) {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
}

static const char* SettingsPageTitle(int page) {
    switch (page) {
        case kSettingsRotation:
            return LangString_BOOK_ORIENT_TITLE;
        case kSettingsFooter:
            return LangString_BOOK_CHAPTER_PROGRESS;
        case kSettingsTapZones:
            return LangString_BOOK_TAP_ZONES_TITLE;
        case kSettingsBackground:
            return LangString_BOOK_BACKGROUND_TITLE;
        case kSettingsWriting:
            return LangString_BOOK_WRITING_TITLE;
        case kSettingsAntialias:
            return LangString_BOOK_ANTIALIAS;
        case kSettingsSpacing:
            return LangString_BOOK_SPACING_TITLE;
        case kSettingsUnderline:
            return LangString_BOOK_UNDERLINE;
        case kSettingsTurning:
            return LangString_BOOK_TURN_TITLE;
        default:
            return LangString_BOOK_READING_SETTINGS;
    }
}

static int TextValue(BookTextOption option) {
    const int value = BookReaderPrefsTextValue(option);
    if (value >= 0) {
        return value;
    }
    switch (option) {
        case BookTextOption::ParaGap:
            return BookReaderPrefsParaGap();
        case BookTextOption::LineGap:
            return (BookFont()->line_height * BookReaderPrefsLineSpacingPercent() + 50) / 100;
        case BookTextOption::Margin:
            return BookReaderPrefsMarginLeft();
        case BookTextOption::MarginTop:
            return BookReaderPrefsMarginTop();
        case BookTextOption::MarginBottom:
            return BookReaderPrefsMarginBottom();
        case BookTextOption::MarginLeft:
            return BookReaderPrefsMarginLeft();
        case BookTextOption::MarginRight:
            return BookReaderPrefsMarginRight();
        default:
            return value;
    }
}

static void RedrawReadingText() {
    if (DeferComicTextSetting()) {
        return;
    }
    CancelReaderPageWork();
    RequestRenderReaderPage();
}

static void CommitNumeric(BookSettingsSheetNumericRow& row) {
    if (!row.active) {
        return;
    }
    row.active = false;
    if (row.value == row.original) {
        return;
    }
    if (row.option == kAdjustFont) {
        if (BookReaderPrefsFontSize() > 0 && PrepareBookFont(BookReaderPrefsFontFile(), row.value)) {
            ScheduleLayoutApply(true);
        }
    } else if (row.option == kAdjustShake) {
        BookReaderPrefsSetShakeLevel(row.value);
    } else if (row.option == kAdjustAutoTurn) {
        BookReaderPrefsSetAutoTurnSeconds(row.value);
        ResetReaderAutoTurnDeadline();
    } else {
        BookReaderPrefsSetTextValue(static_cast<BookTextOption>(row.option), row.value);
        if (row.option == static_cast<int>(BookTextOption::UnderlineOffset)) {
            RedrawReadingText();
        } else {
            ScheduleLayoutApply(false);
        }
    }
}

static void NumericEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const size_t index = static_cast<size_t>(std::abs(encoded) - 1);
    if (index >= s_numeric_rows.size()) {
        return;
    }
    auto& row = s_numeric_rows[index];
    const auto code = lv_event_get_code(event);
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (code == LV_EVENT_PRESSED) {
        row.original = row.value;
        row.active = true;
        lv_obj_set_style_bg_color(button, UiColorForeground(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), UiColorBackground(), 0);
        EpdRequestBlackEqualization();
    }
    if (row.active && (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT)) {
        row.value = std::clamp(row.value + (encoded > 0 ? row.step : -row.step), row.minimum, row.maximum);
        UpdateNumericValue(row);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), UiColorForeground(), 0);
        CommitNumeric(row);
        UpdateNumericBounds(row);
        EpdRequestBlackEqualization();
        if (s_numeric_refresh_pending && !NumericAdjustmentActive()) {
            s_numeric_refresh_pending = false;
            RequestSettingsSheetPaint();
        }
    }
}

static void AddNumericRow(lv_obj_t* parent, const char* title, int option, int value, int minimum, int maximum,
                          int step, bool enabled = true, int column = 0, int row_height = 0) {
    lv_obj_update_layout(parent);
    const int width = lv_obj_get_content_width(parent);
    const int height = std::max(row_height, SettingsRowHeight());
    const int button_height = std::max<int>(UiSy(40), ScreenTextHeight(Book_ItemFont()) + 12);
    const int start = column > 0 ? column : (title && *title ? SettingsColumn(parent, {title}) : 0);
    const int gap = UiSx(8);
    const int value_width =
        std::max<int>(static_cast<int>(UiSx(88)), static_cast<int>(ScreenMeasureTextWidth(Book_ItemFont(), "-00")) +
                                                      static_cast<int>(ScreenMeasureTextWidth(Book_ItemFont(), "px")) +
                                                      static_cast<int>(UiSx(16)));
    const int button_width =
        std::max<int>(1, (static_cast<int>(width) - start - value_width - 2 * static_cast<int>(gap)) / 2);
    auto* row = SheetBox(parent, width, height);
    if (title && *title) {
        auto* caption = SheetLabel(row, title);
        if (start > 0) {
            PlaceSettingsCaption(caption, start);
        }
    }
    const int index = static_cast<int>(s_numeric_rows.size());
    auto* dec = SheetButton(row, option == kAdjustFont ? LangString_BOOK_FONT_DECREASE : LangString_BOOK_DECREASE,
                            button_width, button_height, NumericEvent, -index - 1, false, UiSx(7));
    auto* val = SheetButton(row, "", value_width, button_height, nullptr, 0, true, UiSx(7));
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dec, start, (height - button_height) / 2);
    lv_obj_set_pos(val, start + button_width + gap, (height - button_height) / 2);
    auto* inc = SheetButton(row, option == kAdjustFont ? LangString_BOOK_FONT_INCREASE : LangString_BOOK_INCREASE,
                            button_width, button_height, NumericEvent, index + 1, false, UiSx(7));
    lv_obj_set_pos(inc, start + button_width + value_width + 2 * gap, (height - button_height) / 2);
    auto* label = lv_obj_get_child(val, 0);
    const char* unit_text = "px";
    if (option == kAdjustShake) {
        unit_text = "";
    } else if (option == kAdjustAutoTurn) {
        unit_text = "s";
    }
    auto* unit = SheetLabel(val, unit_text, Book_ItemFont());
    lv_obj_set_style_text_color(unit, UiColorBackground(), 0);
    s_numeric_rows.push_back({option, value, value, minimum, maximum, step, label, false, unit, dec, inc, enabled});
    UpdateNumericValue(s_numeric_rows.back());
    for (auto* button : {dec, inc}) {
        lv_obj_remove_event_cb(button, NumericEvent);
        const intptr_t binding = button == dec ? -index - 1 : index + 1;
        lv_obj_add_event_cb(button, NumericEvent, LV_EVENT_ALL, reinterpret_cast<void*>(binding));
        lv_obj_set_style_border_width(button, 0, LV_STATE_DISABLED);
        lv_obj_add_event_cb(button, ScreenDrawDisabledBorder, LV_EVENT_DRAW_MAIN_END, nullptr);
    }
    UpdateNumericBounds(s_numeric_rows.back());
}

static void OnMarginSide(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const auto option = static_cast<BookTextOption>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (option == s_margin_option) {
        return;
    }
    s_margin_option = option;
    RequestSettingsSheetPaint();
}

static void SettingsNavigate(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    auto& st = Book_State();
    st.settings.reader_settings_page = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    st.tap.tap_zone_pick_cell = -1;
    st.settings.reader_scroll[3] = 0;
    RebuildSettingsSheet();
}

static void SettingsBack(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    auto& st = Book_State();
    if (st.settings.reader_tab == 3 && st.settings.reader_settings_page != kSettingsHome) {
        if (st.settings.reader_settings_page == kSettingsTapZones && TapZonePickOpen()) {
            st.tap.tap_zone_pick_cell = -1;
        } else {
            st.settings.reader_settings_page = kSettingsHome;
            st.tap.tap_zone_pick_cell = -1;
        }
        st.settings.reader_scroll[3] = 0;
        RebuildSettingsSheet();
    } else {
        SheetClose(event);
    }
}

static void SettingsReset(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (Book_State().settings.reader_tab == 1) {
        if (PrepareBookFont(kBookReaderDefaultFontFile, 0)) {
            ScheduleLayoutApply(true);
        }
    } else {
        SetReaderAutoTurnEnabled(false);
        BookTapZonesResetDefaults();
        BookReaderPrefsResetReading();
        EpdSetReaderEffect(EpdEffect::None);
        SyncReaderAutoOrient();
        if (DisplayUiGetOrient() != kBookReaderOrientPortrait) {
            ApplyReaderOrient(kBookReaderOrientPortrait);
        } else {
            ScheduleLayoutApply(false);
        }
        SyncFooterClockTimer();
    }
    RequestSettingsSheetPaint();
}

static void TextChoice(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    BookReaderPrefsSetTextValue(encoded >= 10 ? BookTextOption::AaStrength : BookTextOption::Indent, encoded % 10);
    RequestSettingsSheetPaint();
    if (encoded >= 10) {
        RedrawReadingText();
    } else {
        ScheduleLayoutApply(false);
    }
}

static void SheetShakeTurnToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookReaderPrefsSetShakeTurn(!BookReaderPrefsShakeTurn());
    SyncReaderAutoOrient();
    RequestSettingsSheetPaint();
}

static void SheetAutoTurnToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    SetReaderAutoTurnEnabled(!ReaderAutoTurnEnabled());
    RequestSettingsSheetPaint();
}

static void SheetTextAlign(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int align = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (align == BookReaderPrefsTextAlign()) {
        return;
    }
    BookReaderPrefsSetTextAlign(align);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    ScheduleLayoutApply(false);
}

static void SheetWritingMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode == BookReaderPrefsWritingMode()) {
        return;
    }
    BookReaderPrefsSetWritingMode(mode);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    ScheduleLayoutApply(false);
}

static void SheetPageTurn(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode == BookReaderPrefsPageTurn()) {
        return;
    }
    BookReaderPrefsSetPageTurn(mode);
    EpdSetReaderEffect(mode == kBookReaderPageTurnWipe     ? EpdEffect::Wipe
                       : mode == kBookReaderPageTurnRipple ? EpdEffect::Ripple
                                                           : EpdEffect::None);
    RequestSettingsSheetPaint();
}

static void SheetTextDirection(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int direction = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (direction == BookReaderPrefsTextDirection()) {
        return;
    }
    BookReaderPrefsSetTextDirection(direction);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    RequestRenderReaderPage();
}

static void SheetBackground(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode == 3) {
        OpenReadingWallpaper();
        return;
    }
    BookReaderPrefsSetBackground(BookReaderPrefsBackground() == mode ? 0 : mode);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    CancelReaderPageWork();
    RequestRenderReaderPage();
}

static void SheetInlineImagesToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookReaderPrefsSetInlineImages(!BookReaderPrefsInlineImages());
    RequestSettingsSheetPaint();
    const auto& session = Book_State().reader.session;
    if (session && session->Info().format == BookFormat::kTxt) {
        return;
    }
    ScheduleLayoutApply(false);
}

static void SheetAntialias(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int enabled = !BookReaderPrefsAntialias();
    BookReaderPrefsSetAntialias(enabled);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    CancelReaderPageWork();
    RequestRenderReaderPage();
}

struct BookSettingsSheetSwitchRow {
    lv_obj_t* button = nullptr;
    lv_obj_t* knob = nullptr;
    lv_event_cb_t event = nullptr;
    int button_w = 0;
    int displayed = -1;
};

static std::vector<BookSettingsSheetSwitchRow> s_switch_rows;

static void PlaceSwitchCaption(lv_obj_t* caption, int button_w) {
    const int toggle_space = static_cast<int>(UiSx(8)) + static_cast<int>(UiSx(34)) + static_cast<int>(UiSx(8));
    const int text_w = std::max<int>(1, button_w - toggle_space);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(caption, text_w);
    lv_obj_set_height(caption, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, 0);
}

static bool SettingsSwitchOn(lv_event_cb_t event) {
    if (event == SheetInlineImagesToggle) {
        return BookReaderPrefsInlineImages() != 0;
    }
    if (event == SheetAntialias) {
        return BookReaderPrefsAntialias() != 0;
    }
    if (event == SheetShakeTurnToggle) {
        return BookReaderPrefsShakeTurn() != 0;
    }
    return ReaderAutoTurnEnabled();
}

static void RefreshSwitchRows() {
    for (auto& row : s_switch_rows) {
        const bool on = SettingsSwitchOn(row.event);
        auto* caption = lv_obj_get_child(row.button, 0);
        const char* text = on ? LangString_COMMON_ON : LangString_COMMON_OFF;
        if (row.displayed == static_cast<int>(on)) {
            continue;
        }
        row.displayed = on;
        lv_label_set_text(caption, text);
        PlaceSwitchCaption(caption, row.button_w);
        lv_obj_set_style_bg_color(row.button, UiColorBackground(), 0);
        lv_obj_set_style_text_color(caption, UiColorForeground(), 0);
        auto* track = lv_obj_get_parent(row.knob);
        lv_obj_set_style_bg_color(track, on ? UiColorForeground() : UiColorBackground(), 0);
        lv_obj_set_style_bg_color(row.knob, on ? UiColorBackground() : UiColorForeground(), 0);
        lv_obj_set_style_border_color(track, UiColorForeground(), 0);
        lv_obj_align(row.knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        lv_obj_move_foreground(track);
        if (on) {
            EpdRequestBlackEqualization();
        }
    }
}

static void AddSwitchRow(lv_obj_t* body, const char* title, lv_event_cb_t event, int column = 0) {
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body), height = SettingsRowHeight();
    const int start = column > 0 ? column : SettingsColumn(body, {title});
    const int button_w = std::max<int>(1, static_cast<int>(width) - start);
    const bool on = SettingsSwitchOn(event);
    auto* row = SheetBox(body, width, height);
    auto* label = SheetLabel(row, title);
    PlaceSettingsCaption(label, start);
    const int button_height = std::max<int>(UiSy(40), ScreenTextHeight(Book_ItemFont()) + 12);
    auto* button = SheetButton(row, on ? LangString_COMMON_ON : LangString_COMMON_OFF, button_w, button_height, event,
                               0, false, UiSx(7));
    lv_obj_align(button, LV_ALIGN_LEFT_MID, start, 0);
    PlaceSwitchCaption(lv_obj_get_child(button, 0), button_w);
    auto* track = SheetBox(button, UiSx(34), UiSx(19));
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(track, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, -UiSx(8), 0);
    auto* knob = SheetBox(track, UiSx(14), UiSx(14));
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_y(knob, UiSy(2));
    s_switch_rows.push_back({button, knob, event, button_w});
    RefreshSwitchRows();
}

static const char* SheetTurnSummary() {
    if (ReaderAutoTurnEnabled()) {
        return LangString_BOOK_AUTO_TURN;
    }
    if (BookReaderPrefsShakeTurn()) {
        return LangString_BOOK_SHAKE_TURN;
    }
    if (BookReaderPrefsPageTurn() == kBookReaderPageTurnWipe) {
        return LangString_BOOK_TURN_PUSH;
    }
    return LangString_BOOK_TURN_SLIDE;
}

static void BuildSheetSettings(lv_obj_t* body) {
    auto& settings = Book_State().settings;
    const int page = settings.reader_settings_page;
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body);
    if (page == kSettingsTapZones) {
        BuildTapZoneSettings(body, SettingsGridContentHeight());
        return;
    }
    if (page == kSettingsHome) {
        const int pages[] = {kSettingsRotation,   kSettingsFooter,    kSettingsTapZones,
                             kSettingsBackground, kSettingsWriting,   kSettingsAntialias,
                             kSettingsSpacing,    kSettingsUnderline, kSettingsTurning};
        const char* titles[] = {
            LangString_BOOK_ORIENT_TITLE,     LangString_BOOK_CHAPTER_PROGRESS, LangString_BOOK_TAP_ZONES_TITLE,
            LangString_BOOK_BACKGROUND_TITLE, LangString_BOOK_WRITING_TITLE,    LangString_BOOK_ANTIALIAS,
            LangString_BOOK_SPACING_TITLE,    LangString_BOOK_UNDERLINE,        LangString_BOOK_TURN_TITLE};
        const char* orientation =
            BookReaderPrefsOrient() == kBookReaderOrientAuto        ? LangString_BOOK_ORIENT_AUTO
            : BookReaderPrefsOrient() == kBookReaderOrientLandLeft  ? LangString_BOOK_ORIENT_LAND_LEFT
            : BookReaderPrefsOrient() == kBookReaderOrientLandRight ? LangString_BOOK_ORIENT_LAND_RIGHT
                                                                    : LangString_BOOK_ORIENT_PORTRAIT;
        const int bg = BookReaderPrefsBackground();
        const char* values[] = {
            orientation,
            BookReaderPrefsFooterLayout() == kBookReaderFooterSplit ? LangString_BOOK_FOOTER_SPLIT
                                                                   : LangString_BOOK_FOOTER_BOTTOM,
            BookTapZonesIsDefault() ? LangString_BOOK_DEFAULT : LangString_BOOK_CUSTOM,
            bg == 0   ? LangString_COMMON_OFF
            : bg == 1 ? LangString_BOOK_BACKGROUND_GRID
            : bg == 2 ? LangString_BOOK_BACKGROUND_DOTS
                      : LangString_BOOK_CUSTOM,
            BookReaderPrefsWritingMode() ? LangString_BOOK_WRITING_VERTICAL : LangString_BOOK_WRITING_HORIZONTAL,
            BookReaderPrefsAntialias() ? (TextValue(BookTextOption::AaStrength) == 0   ? LangString_BOOK_AA_LOW
                                          : TextValue(BookTextOption::AaStrength) == 1 ? LangString_BOOK_AA_MEDIUM
                                                                                       : LangString_BOOK_AA_HIGH)
                                       : LangString_COMMON_OFF,
            LangString_BOOK_CUSTOM,
            BookReaderPrefsUnderlineMode() == 0   ? LangString_BOOK_TAP_ZONE_NONE
            : BookReaderPrefsUnderlineMode() == 1 ? LangString_BOOK_UNDERLINE_SOLID
                                                  : LangString_BOOK_UNDERLINE_DASHED,
            SheetTurnSummary()};
        const int gap = UiSx(10), card_w = (width - 2 * gap) / 3, card_h = SettingsCardHeight();
        lv_obj_set_layout(body, LV_LAYOUT_NONE);
        for (int i = 0; i < 9; ++i) {
            auto* card = SheetButton(body, "", card_w, card_h, SettingsNavigate, pages[i], false, UiSx(10));
            lv_obj_set_pos(card, (i % 3) * (card_w + gap), (i / 3) * (card_h + UiSy(9)));
            auto* title = lv_obj_get_child(card, 0);
            lv_label_set_text(title, titles[i]);
            ScreenFitLabel(title, card_w - UiSx(20), ScreenTextHeight(Book_ItemFont()), Book_ItemFont());
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, UiSx(10), UiSy(11));
            auto* value = SheetLabel(card, values[i], UiFont(UiFontRole::Emphasis));
            ScreenFitLabel(value, card_w - UiSx(30), ScreenTextHeight(UiFont(UiFontRole::Emphasis)),
                           UiFont(UiFontRole::Emphasis));
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, UiSx(10), -UiSy(10));
            auto* arrow = SheetLabel(card, ">");
            lv_obj_align(arrow, LV_ALIGN_BOTTOM_RIGHT, -UiSx(8), -UiSy(8));
        }
        return;
    }
    ConfigureSheetScroll(body);
    if (page == kSettingsRotation || page == kSettingsBackground) {
        const bool background = page == kSettingsBackground;
        const char* orientation_labels[] = {LangString_BOOK_ORIENT_PORTRAIT, LangString_BOOK_ORIENT_LAND_LEFT,
                                            LangString_BOOK_ORIENT_LAND_RIGHT, LangString_BOOK_ORIENT_AUTO};
        const int orientation_values[] = {kBookReaderOrientPortrait, kBookReaderOrientLandLeft,
                                          kBookReaderOrientLandRight, kBookReaderOrientAuto};
        const char* background_labels[] = {LangString_COMMON_OFF, LangString_BOOK_BACKGROUND_GRID,
                                           LangString_BOOK_BACKGROUND_DOTS, LangString_BOOK_CUSTOM};
        const int background_values[] = {0, 1, 2, 3};
        const auto* labels = background ? background_labels : orientation_labels;
        const auto* values = background ? background_values : orientation_values;
        const auto event = background ? SheetBackground : OnSheetOrientMode;
        const int selected = background ? BookReaderPrefsBackground() : BookReaderPrefsOrient();
        const int gap = UiSx(8), w = (width - 3 * gap) / 4;
        auto* row = SheetBox(body, width, SettingsPreviewButtonHeight() + UiSy(20));
        for (int i = 0; i < 4; ++i) {
            auto* button = SheetButton(row, labels[i], w, SettingsPreviewButtonHeight(), event, values[i],
                                       selected == values[i], UiSx(7));
            lv_obj_set_pos(button, i * (w + gap), UiSy(10));
            s_sheet_bindings.push_back({button, event, values[i], false});
        }
    } else if (page == kSettingsFooter) {
        const int column = SettingsColumn(body, {LangString_BOOK_CHAPTER_PROGRESS, LangString_BOOK_FOOTER_LAYOUT});
        const char* fields[] = {LangString_BOOK_FOOTER_TITLE, LangString_BOOK_FOOTER_TIME,
                                LangString_BOOK_FOOTER_PROGRESS, LangString_BOOK_FOOTER_BATTERY};
        const int bits[] = {kBookReaderFooterTitle, kBookReaderFooterTime, kBookReaderFooterProgress,
                            kBookReaderFooterBattery};
        SettingsRow(body, LangString_BOOK_CHAPTER_PROGRESS, 4, fields, OnSheetFooterBitToggle, bits,
                    BookReaderPrefsFooterMask(), true, 0, column);
        const char* layouts[] = {LangString_BOOK_FOOTER_BOTTOM, LangString_BOOK_FOOTER_SPLIT};
        const int values[] = {kBookReaderFooterBottom, kBookReaderFooterSplit};
        SettingsRow(body, LangString_BOOK_FOOTER_LAYOUT, 2, layouts, OnSheetFooterLayout, values,
                    BookReaderPrefsFooterLayout(), false, 0, column);
    } else if (page == kSettingsWriting) {
        const int column = SettingsColumn(body, {LangString_BOOK_TEXT_ALIGN, LangString_BOOK_WRITING_TITLE,
                                                 LangString_BOOK_TEXT_DIRECTION, LangString_BOOK_INLINE_IMAGES});
        const char* align_labels[] = {LangString_BOOK_TEXT_ALIGN_LEFT, LangString_BOOK_TEXT_ALIGN_JUSTIFY};
        const int align_values[] = {kBookReaderTextAlignLeft, kBookReaderTextAlignJustify};
        SettingsRow(body, LangString_BOOK_TEXT_ALIGN, 2, align_labels, SheetTextAlign, align_values,
                    BookReaderPrefsTextAlign(), false, 0, column);
        const char* labels[] = {LangString_BOOK_WRITING_HORIZONTAL, LangString_BOOK_WRITING_VERTICAL};
        const int values[] = {0, 1};
        SettingsRow(body, LangString_BOOK_WRITING_TITLE, 2, labels, SheetWritingMode, values,
                    BookReaderPrefsWritingMode(), false, 0, column);
        if (BookReaderPrefsWritingMode()) {
            const char* directions[] = {LangString_BOOK_TEXT_LEFT_TO_RIGHT, LangString_BOOK_TEXT_RIGHT_TO_LEFT};
            SettingsRow(body, LangString_BOOK_TEXT_DIRECTION, 2, directions, SheetTextDirection, values,
                        BookReaderPrefsTextDirection(), false, 0, column);
        }
        AddSwitchRow(body, LangString_BOOK_INLINE_IMAGES, SheetInlineImagesToggle, column);
    } else if (page == kSettingsAntialias) {
        const int column = SettingsColumn(body, {LangString_BOOK_ANTIALIAS, LangString_BOOK_AA_STRENGTH});
        AddSwitchRow(body, LangString_BOOK_ANTIALIAS, SheetAntialias, column);
        const char* strengths[] = {LangString_BOOK_AA_LOW, LangString_BOOK_AA_MEDIUM, LangString_BOOK_AA_HIGH};
        const int levels[] = {10, 11, 12};
        SettingsRow(body, LangString_BOOK_AA_STRENGTH, 3, strengths, TextChoice, levels,
                    10 + TextValue(BookTextOption::AaStrength), false, 0, column);
    } else if (page == kSettingsSpacing) {
        const int row_height = SettingsRowHeight();
        const int column =
            SettingsColumn(body, {LangString_BOOK_PARA_GAP_TITLE, LangString_BOOK_LETTER_GAP, LangString_BOOK_LINE_GAP,
                                  LangString_BOOK_MARGIN, LangString_BOOK_INDENT});
        AddNumericRow(body, LangString_BOOK_PARA_GAP_TITLE, static_cast<int>(BookTextOption::ParaGap),
                      TextValue(BookTextOption::ParaGap), BOARD_READER_PARA_GAP_MIN, BOARD_READER_PARA_GAP_MAX, 2, true,
                      column, row_height);
        AddNumericRow(body, LangString_BOOK_LETTER_GAP, static_cast<int>(BookTextOption::LetterGap),
                      TextValue(BookTextOption::LetterGap), -2, 12, 1, true, column, row_height);
        AddNumericRow(body, LangString_BOOK_LINE_GAP, static_cast<int>(BookTextOption::LineGap),
                      TextValue(BookTextOption::LineGap), BOARD_READER_LINE_GAP_MIN, BOARD_READER_LINE_GAP_MAX, 1, true,
                      column, row_height);
        const char* sides[] = {LangString_BOOK_MARGIN_TOP, LangString_BOOK_MARGIN_BOTTOM, LangString_BOOK_MARGIN_LEFT,
                               LangString_BOOK_MARGIN_RIGHT};
        const int options[] = {
            static_cast<int>(BookTextOption::MarginTop), static_cast<int>(BookTextOption::MarginBottom),
            static_cast<int>(BookTextOption::MarginLeft), static_cast<int>(BookTextOption::MarginRight)};
        auto* margin = SheetBox(body, width, row_height * 2);
        lv_obj_set_height(margin, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(margin, LV_FLEX_FLOW_COLUMN);
        auto* caption = SheetLabel(margin, LangString_BOOK_MARGIN);
        lv_obj_add_flag(caption, LV_OBJ_FLAG_FLOATING);
        PlaceSettingsCaption(caption, column);
        SettingsRow(margin, "", 4, sides, OnMarginSide, options, static_cast<int>(s_margin_option), false, 0, column,
                    row_height);
        AddNumericRow(margin, "", static_cast<int>(s_margin_option), TextValue(s_margin_option),
                      BOARD_READER_MARGIN_MIN, BOARD_READER_MARGIN_MAX, 2, true, column, row_height);
        const char* labels[] = {LangString_BOOK_TAP_ZONE_NONE, LangString_BOOK_INDENT_ONE, LangString_BOOK_INDENT_TWO,
                                LangString_BOOK_INDENT_THREE};
        const int values[] = {0, 1, 2, 3};
        SettingsRow(body, LangString_BOOK_INDENT, 4, labels, TextChoice, values, TextValue(BookTextOption::Indent),
                    false, 0, column, row_height);
    } else if (page == kSettingsUnderline) {
        const int column = SettingsColumn(body, {LangString_BOOK_UNDERLINE, LangString_BOOK_UNDERLINE_POSITION});
        const char* labels[] = {LangString_BOOK_TAP_ZONE_NONE, LangString_BOOK_UNDERLINE_SOLID,
                                LangString_BOOK_UNDERLINE_DASHED};
        const int values[] = {0, 1, 2};
        SettingsRow(body, LangString_BOOK_UNDERLINE, 3, labels, OnSheetUnderlineMode, values,
                    BookReaderPrefsUnderlineMode(), false, 0, column);
        AddNumericRow(body, LangString_BOOK_UNDERLINE_POSITION, static_cast<int>(BookTextOption::UnderlineOffset),
                      TextValue(BookTextOption::UnderlineOffset), -12, 12, 2, true, column);
    } else if (page == kSettingsTurning) {
        const int column = SettingsColumn(body, {LangString_BOOK_TURN_TITLE, LangString_BOOK_AUTO_TURN,
                                                 LangString_BOOK_TURN_INTERVAL, LangString_BOOK_SHAKE_TURN,
                                                 LangString_BOOK_SHAKE_LEVEL});
        const char* labels[] = {LangString_BOOK_TURN_SLIDE, LangString_BOOK_TURN_PUSH};
        const int values[] = {kBookReaderPageTurnSlide, kBookReaderPageTurnWipe};
        SettingsRow(body, LangString_BOOK_TURN_TITLE, BOARD_SUPPORTS_PAGE_WIPE ? 2 : 1, labels, SheetPageTurn, values,
                    BookReaderPrefsPageTurn(), false, 0, column);
        AddSwitchRow(body, LangString_BOOK_SHAKE_TURN, SheetShakeTurnToggle, column);
        AddNumericRow(body, LangString_BOOK_SHAKE_LEVEL, kAdjustShake, BookReaderPrefsShakeLevel(), 1, 5, 1, true,
                      column);
        AddSwitchRow(body, LangString_BOOK_AUTO_TURN, SheetAutoTurnToggle, column);
        AddNumericRow(body, LangString_BOOK_TURN_INTERVAL, kAdjustAutoTurn, BookReaderPrefsAutoTurnSeconds(),
                      BOARD_READER_AUTO_TURN_MIN_SECONDS, BOARD_READER_AUTO_TURN_MAX_SECONDS, 1, true, column);
    }
}

static int s_settings_body_height = 0;  // 当前设置页完成换行后的实际高度

// Only viewport rows exist; a one-pixel extent preserves the full scroll range.
static lv_obj_t* s_toc_list = nullptr;
static std::vector<lv_obj_t*> s_toc_rows;
static int s_toc_page_rows = BOARD_READER_TOC_ROWS;  // 当前目录内容区可容纳的行数
static int s_toc_height = 1, s_toc_first = -1;
static bool s_toc_binding = false;
static void BindTocRows();

static void TocScroll(lv_event_t*) {
    BindTocRows();
}

static ScreenFixedFooter s_toc_frame;
static lv_obj_t* s_toc_previous = nullptr;
static lv_obj_t* s_toc_next = nullptr;

static void SheetListPageState(lv_event_t* e) {
    auto* list = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!s_toc_frame.Root() || !s_toc_previous || !s_toc_next) {
        return;
    }
    const int current = lv_obj_get_scroll_y(list);
    const bool previous = current > 0;
    const bool next = current + lv_obj_get_height(list) < s_toc_items * s_toc_height;
    SetSheetBtnEnabled(s_toc_previous, previous);
    SetSheetBtnEnabled(s_toc_next, next);
}

struct BookSettingsSheetSheetPageGesture {
    lv_obj_t* list = nullptr;
    int stride = 1;
    int pages = 1;
};

static BookSettingsSheetSheetPageGesture s_font_pages, s_toc_pages;

static void ChangeSheetListPage(BookSettingsSheetSheetPageGesture& pages, int direction) {
    ScreenScrollbarShow(pages.list);
    if (&pages == &s_font_pages) {
        auto& settings = Book_State().settings;
        if (settings.font_scan_loading) {
            return;
        }
        const int page = settings.font_list_page + direction;
        if (page < 0 || page >= FontListPageCount()) {
            return;
        }
        settings.font_list_page = page;
        StartReadFontScan();
        RequestSettingsSheetPaint();
        return;
    }
    if (!pages.list || pages.pages <= 1) {
        return;
    }
    const int current = lv_obj_get_scroll_y(pages.list) / pages.stride;
    const int target = std::clamp(current + direction, 0, pages.pages - 1);
    if (target == current) {
        return;
    }
    lv_obj_scroll_to_y(pages.list, target * pages.stride, LV_ANIM_OFF);
    auto& settings = Book_State().settings;
    settings.reader_scroll[settings.reader_tab] = lv_obj_get_scroll_y(pages.list);
}

static void SheetListGesture(lv_event_t* event) {
    auto* pages = static_cast<BookSettingsSheetSheetPageGesture*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (!pages || !indev) {
        return;
    }
    const auto direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM) {
        return;
    }
    lv_event_stop_bubbling(event);
    lv_indev_wait_release(indev);
    ChangeSheetListPage(*pages, direction == LV_DIR_TOP ? 1 : -1);
}

static void DrawSheetPageIndicator(lv_event_t* event) {
    const auto* pages = static_cast<const BookSettingsSheetSheetPageGesture*>(lv_event_get_user_data(event));
    if (!pages) {
        return;
    }
    const int count = pages == &s_font_pages ? FontListPageCount() : pages->pages;
    if (count <= 1 || !ScreenScrollbarVisible(pages->list)) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(pages->list, &area);
    const int inset = UiSy(4);
    const int track = std::max<int>(1, lv_area_get_height(&area) - 2 * inset);
    const int length = std::min(track, std::max<int>(UiSy(12), track / count));
    const int page = pages == &s_font_pages
                         ? Book_State().settings.font_list_page
                         : std::clamp<int>(lv_obj_get_scroll_y(pages->list) / pages->stride, 0, count - 1);
    area.y1 += inset + (track - length) * page / (count - 1);
    area.y2 = area.y1 + length - 1;
    area.x2 -= 1;
    area.x1 = area.x2 - UiSx(5) + 1;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = UiColorForeground();
    bar.bg_opa = LV_OPA_COVER;
    bar.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(lv_event_get_layer(event), &bar, &area);
}

static void ConfigureSheetPaging(lv_obj_t* list, BookSettingsSheetSheetPageGesture& pages, int stride, int count) {
    pages = {list, std::max(1, stride), std::max(1, count)};
    lv_obj_remove_event_cb(list, SheetScrollBegin);
    lv_obj_remove_event_cb(list, SheetScrollMove);
    lv_obj_remove_event_cb(list, SheetScrollEnd);
    Book_DisableScroll(list);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    if (pages.pages > 1) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(list, kTouchUiPagedVerticalDrag);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(list, SheetListGesture, LV_EVENT_GESTURE, &pages);
        lv_obj_add_event_cb(list, DrawSheetPageIndicator, LV_EVENT_DRAW_POST_END, &pages);
        ScreenScrollbarAttach(list);
    }
}

static void SheetListPage(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    ChangeSheetListPage(s_toc_pages, reinterpret_cast<intptr_t>(lv_event_get_user_data(event)) > 0 ? 1 : -1);
}

static int TocNavigationHeight() {
    return std::max<int>(UiSy(44), ScreenTextHeight() + UiSy(16));
}

static lv_obj_t* SheetPagedList(lv_obj_t* parent, int width, int height) {
    const int nav = TocNavigationHeight();
    s_toc_frame.Create(parent, width, std::max(1, height - 6), nav, UiColorForeground());
    lv_obj_set_style_radius(s_toc_frame.Root(), UiSx(6), 0);
    lv_obj_set_style_clip_corner(s_toc_frame.Root(), true, 0);
    auto* list = s_toc_frame.Content();
    auto* footer = s_toc_frame.Footer();
    ConfigureSheetScroll(list);
    const int button_width = std::min<int>(UiSx(88), lv_obj_get_content_width(footer) / 2);
    const int button_height = lv_obj_get_content_height(footer);
    s_toc_previous = SheetButton(footer, "<<", button_width, button_height, SheetListPage, -1);
    s_toc_next = SheetButton(footer, ">>", button_width, button_height, SheetListPage, 1);
    for (auto* button : {s_toc_previous, s_toc_next}) {
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_radius(button, 0, 0);
    }
    lv_obj_align(s_toc_previous, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(s_toc_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(list, SheetListPageState, LV_EVENT_SCROLL, nullptr);
    return list;
}

static int FontGridCardHeight() {
    return ScreenTextHeight(Book_ItemFont(), 2, UiSy(2)) + 2 * UiSy(4) + 4;
}

static int FontSizeAdjustHeight() {
    return SettingsRowHeight();
}

static int FontGridBodyHeight() {
    const int frame = 3 * FontGridCardHeight() + 4 * UiSx(10) + 8;
    const int adjust = Book_State().settings.font_multi ? 0 : FontSizeAdjustHeight();
    return frame + adjust + UiSy(10) + 4 + 2 * ReadingSheetBodyPadding();
}

static void BuildSheetFonts(lv_obj_t* body) {
    auto& st = Book_State();
    s_live_font_revision = st.settings.font_list_revision;
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body), gap = UiSx(10);
    const int adjust_height = st.settings.font_multi ? 0 : FontSizeAdjustHeight();
    const int height = std::max<int>(UiSy(90), lv_obj_get_content_height(body) - adjust_height - UiSy(10) - 4);
    auto* frame = SheetBox(body, width, height);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, UiColorForeground(), 0);
    lv_obj_set_style_border_post(frame, true, 0);
    lv_obj_set_style_radius(frame, UiSx(8), 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    auto* list = SheetBox(frame, width - 4, height - 8);
    lv_obj_set_style_pad_all(list, gap, 0);
    Book_DisableScroll(list);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_update_layout(list);
    const int card_w = (lv_obj_get_content_width(list) - 2 * gap) / 3;
    const int card_h = std::max<int>(1, (lv_obj_get_content_height(list) - 2 * gap) / 3);
    // 面板达到屏幕高度上限时，按实际行空间适配字号，保持每页九项。
    const int text_height =
        std::max<int>(1, std::min<int>(ScreenTextHeight(Book_ItemFont()), (card_h - 2 * UiSy(4) - UiSy(2) - 4) / 2));
    const bool show_loading =
        st.settings.font_scan_loading &&
        (st.settings.font_entries.empty() || esp_timer_get_time() - st.settings.font_requested_us >= 180000);
    if (show_loading || st.settings.font_scan_failed || st.settings.font_entries.empty()) {
        Book_DisableScroll(list);
        Book_ShowMessage(list,
                         st.settings.font_scan_loading  ? LangString_BOOK_FONT_LOADING
                         : st.settings.font_scan_failed ? StorageErrorText(st.settings.font_scan_error)
                                                        : LangString_BOOK_FONT_EMPTY,
                         Book_ItemFont());
    } else {
        for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
            const auto& entry = st.settings.font_entries[i];
            const bool active = st.settings.font_multi ? FontItemSelected(i) : entry.file == BookReaderPrefsFontFile();
            auto* card = SheetButton(list, "", card_w, card_h, OnSheetFontSelect, 0, active, UiSx(7));
            lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(card, OnSheetFontLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
            lv_obj_set_pos(card, (i % 3) * (card_w + gap), (i / 3) * (card_h + gap));
            if (i % 9 == 0) {
                lv_obj_add_flag(card, LV_OBJ_FLAG_SNAPPABLE);
            } else {
                lv_obj_remove_flag(card, LV_OBJ_FLAG_SNAPPABLE);
            }
            auto* label = lv_obj_get_child(card, 0);
            char name[SD_FILE_NAME_CAPACITY];
            const std::string basename = entry.file.substr(entry.file.find_last_of('/') + 1);
            FormatFontListName(basename, name, sizeof(name));
            lv_label_set_text(label, name);
            lv_obj_set_width(label, card_w - UiSx(12));
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            ScreenFitLabel(label, card_w - UiSx(12), text_height, Book_ItemFont());
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_height(label, lv_obj_get_style_text_font(label, LV_PART_MAIN)->line_height);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, UiSy(4));
            char meta[32];
            if (IsTtfFileName(entry.file.c_str())) {
                std::snprintf(meta, sizeof(meta), "%s%s",
                              strcasecmp(entry.file.c_str() + entry.file.size() - 4, ".otf") == 0 ? "OTF" : "TTF",
                              st.settings.font_multi && active ? " *" : "");
            } else {
                std::snprintf(meta, sizeof(meta), "EF %dpx%s", entry.size_px,
                              st.settings.font_multi && active ? " *" : "");
            }
            auto* info = SheetLabel(card, meta, Book_ItemFont());
            lv_obj_set_style_text_color(info, active ? UiColorBackground() : UiColorForeground(), 0);
            ScreenFitLabel(info, card_w - UiSx(12), text_height, Book_ItemFont());
            lv_obj_set_height(info, lv_obj_get_style_text_font(info, LV_PART_MAIN)->line_height);
            lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -UiSy(4));
        }
        s_font_pages.list = list;
        s_font_pages.pages = FontListPageCount();
        lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(list, kTouchUiPagedVerticalDrag);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(list, SheetListGesture, LV_EVENT_GESTURE, &s_font_pages);
        lv_obj_add_event_cb(list, DrawSheetPageIndicator, LV_EVENT_DRAW_POST_END, &s_font_pages);
        ScreenScrollbarAttach(list);
    }
    if (!st.settings.font_multi) {
        const bool vector = BookReaderPrefsFontSize() > 0;
        const int index = CurrentFontIndex();
        const int size = vector       ? BookReaderPrefsFontSize()
                         : index >= 0 ? st.settings.font_entries[index].size_px
                                      : BookFont()->line_height;
        AddNumericRow(body, "", kAdjustFont, size, 20, 80, 1, vector, 0, FontSizeAdjustHeight());
    }
}

static bool RefreshSheetFontControls() {
    auto& st = Book_State();
    auto& settings = st.settings;
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone || settings.font_multi ||
        settings.font_scan_loading || settings.font_scan_failed ||
        s_live_font_revision != settings.font_list_revision || !s_font_pages.list ||
        !lv_obj_is_valid(s_font_pages.list) || s_font_pages.pages != FontListPageCount() ||
        lv_obj_get_child_count(s_font_pages.list) != settings.font_entries.size() || s_numeric_rows.size() != 1) {
        return false;
    }
    for (size_t i = 0; i < settings.font_entries.size(); ++i) {
        auto* card = lv_obj_get_child(s_font_pages.list, i);
        const bool selected = settings.font_entries[i].file == BookReaderPrefsFontFile();
        const auto background = selected ? UiColorForeground() : UiColorBackground();
        if (!lv_color_eq(lv_obj_get_style_bg_color(card, LV_PART_MAIN), background)) {
            lv_obj_set_style_bg_color(card, background, 0);
            for (uint32_t child = 0; child < lv_obj_get_child_count(card); ++child) {
                lv_obj_set_style_text_color(lv_obj_get_child(card, child),
                                             selected ? UiColorBackground() : UiColorForeground(), 0);
            }
        }
    }
    if (BookFontReloadBusy() || st.reader.layout_debounce_reload_font || st.reader.layout_reload_font) {
        return true;
    }
    auto& row = s_numeric_rows.front();
    row.adjustable = BookReaderPrefsFontSize() > 0;
    const int index = CurrentFontIndex();
    row.value = row.adjustable ? BookReaderPrefsFontSize()
                : index >= 0 ? settings.font_entries[index].size_px
                             : BookFont()->line_height;
    UpdateNumericValue(row);
    UpdateNumericBounds(row);
    return true;
}

static void BuildSheetToc(lv_obj_t* body) {
    auto& st = Book_State();
    auto& session = *st.reader.session;
    // Chapter metadata is already loaded. Submit the complete card once;
    // flushing a transient hint starts panel power-up during row allocation.
    lv_obj_update_layout(body);
    const bool landscape = LV_HOR_RES > LV_VER_RES;
    const int body_w = lv_obj_get_content_width(body), body_h = lv_obj_get_content_height(body);
    if (landscape) {
        lv_obj_set_layout(body, LV_LAYOUT_NONE);
    }
    const int head_w = landscape ? (body_w - 16) / 2 : body_w;
    const int minimum_row = std::max(Book_ListFont()->line_height, Book_ItemFont()->line_height) + UiSy(8);
    const int minimum_head = Book_ListFont()->line_height + Book_ItemFont()->line_height + UiSy(20);
    const int head_height = landscape
                                ? body_h
                                : std::clamp(body_h - TocNavigationHeight() - 10 - BOARD_READER_TOC_ROWS * minimum_row,
                                             minimum_head, std::max(minimum_head, static_cast<int>(UiSy(184))));
    auto* head = SheetBox(body, head_w, head_height);
    const int ch = landscape ? body_h * 3 / 5 : std::max(1, head_height - static_cast<int>(UiSy(12)));
    const int cw = ch * UiSx(140) / UiSy(172);
    const auto& info = session.Info();
    bool fill = false;
    auto* cover = CreateCoverSlot(head, info, cw, ch, &st.settings.reader_sheet_cover, &fill);
    lv_obj_align(cover, LV_ALIGN_LEFT_MID, 0, 0);
    if (fill) {
        EnqueueListCoverFill(info, CoverFrameInner(cw), CoverFrameInner(ch), cover, &st.settings.reader_sheet_cover);
        ScheduleListCoverFill();
    }
    auto* title = SheetLabel(head, session.Title().c_str(), Book_ListFont());
    const int info_x = cw + UiSx(20);
    const int cover_top = landscape ? (body_h - ch) / 2 : 0;
    lv_obj_set_width(title, head_w - info_x);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, info_x, landscape ? cover_top + ch * 20 / 172 : UiSy(8));
    if (landscape) {
        lv_obj_set_height(title, Book_ListFont()->line_height * 2);
    }
    std::string author_name = session.Author();
    const auto first = author_name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        author_name = LangString_COMMON_UNKNOWN;
    } else {
        author_name = author_name.substr(first, author_name.find_last_not_of(" \t\r\n") - first + 1);
    }
    if (author_name == "未知" || author_name == "未命名" || author_name == "unknown" || author_name == "Unknown" ||
        author_name == "null" || author_name == LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC || author_name == "　") {
        author_name = LangString_COMMON_UNKNOWN;
    }
    auto* author = SheetLabel(head, author_name.c_str());
    lv_obj_set_size(author, head_w - info_x, Book_ItemFont()->line_height);
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(author, info_x,
                   landscape ? cover_top + ch - Book_ItemFont()->line_height - 6
                             : head_height - Book_ItemFont()->line_height - UiSy(8));
    lv_obj_update_layout(body);
    const int width = landscape ? (body_w - 16) / 2 : body_w;
    auto* list = SheetPagedList(body, width, landscape ? body_h : std::max<int>(1, body_h - head_height));
    if (landscape) {
        lv_obj_set_pos(s_toc_frame.Root(), (body_w + 16) / 2, 0);
        auto* divider = SheetBox(body, 2, body_h);
        lv_obj_set_pos(divider, body_w / 2 - 1, 0);
        lv_obj_set_style_bg_color(divider, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    }
    s_toc_list = list;
    s_toc_count = session.TocCount();
    s_toc_home = session.TocStartsAfterFirstPage();
    lv_obj_set_layout(list, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    const int list_height = ScreenContentSize(list).height;
    s_toc_page_rows = std::max(2, UiLayoutCount(list_height, minimum_row, 0, BOARD_READER_TOC_ROWS));
    const int chapters_per_page = s_toc_page_rows - (s_toc_home ? 1 : 0);
    const int home_rows = s_toc_home ? (s_toc_count + chapters_per_page - 1) / chapters_per_page : 0;
    s_toc_items = s_toc_count + home_rows;
    s_toc_building = session.LiveRelayoutPending();
    s_toc_first = -1;
    s_toc_height = std::max(1, list_height / s_toc_page_rows);
    if (!s_toc_count) {
        Book_ShowMessage(list, session.LiveRelayoutPending() ? LangString_BOOK_TOC_BUILDING : LangString_BOOK_TOC_EMPTY,
                         Book_ItemFont());
        return;
    }
    const int viewport = s_toc_page_rows * s_toc_height;
    lv_obj_set_flex_grow(list, 0);
    lv_obj_set_height(list, viewport);
    lv_obj_set_height(s_toc_frame.Root(), viewport + TocNavigationHeight() + 4);
    lv_obj_update_layout(s_toc_frame.Root());
    auto* extent = SheetBox(list, 1, 1);
    lv_obj_set_pos(extent, 0, (s_toc_items - 1) * s_toc_height + viewport - 1);
    const int pool = std::min(s_toc_items, (viewport + s_toc_height - 1) / s_toc_height + 2);
    for (int slot = 0; slot < pool; ++slot) {
        auto* row = SheetButton(list, "", ScreenContentSize(list).width, s_toc_height, SheetTocJump, 0);
        SheetLabel(row, "");
        s_toc_rows.push_back(row);
    }
    if (st.settings.reader_scroll[0] < 0) {
        const int current = std::max(0, session.CurrentTocIndex());
        const int display = s_toc_home ? current / chapters_per_page * s_toc_page_rows : current;
        st.settings.reader_scroll[0] = display * s_toc_height;
    }
    const int rows = std::max(1, viewport / s_toc_height);
    ConfigureSheetPaging(list, s_toc_pages, rows * s_toc_height, (s_toc_items + rows - 1) / rows);
    lv_obj_add_event_cb(list, TocScroll, LV_EVENT_SCROLL, nullptr);
    BindTocRows();
    lv_obj_send_event(list, LV_EVENT_SCROLL, nullptr);
}

static void BindTocRows() {
    if (!s_toc_list || s_toc_binding || !s_toc_count) {
        return;
    }
    const int first = std::clamp<int>(lv_obj_get_scroll_y(s_toc_list) / s_toc_height, 0,
                                      std::max(0, s_toc_items - int(s_toc_rows.size())));
    if (first == s_toc_first) {
        return;
    }
    s_toc_binding = true;
    s_toc_first = first;
    auto& session = *Book_State().reader.session;
    const int width = ScreenContentSize(s_toc_list).width;
    const int current_chapter = session.CurrentTocIndex();
    const bool at_home = s_toc_home && session.ChapterIndex() == 0 && session.CurrentPage() == 0;
    const char* home_title = s_toc_home ? session.TocHomeTitle() : nullptr;
    for (size_t slot = 0; slot < s_toc_rows.size(); ++slot) {
        const int display = first + static_cast<int>(slot);
        const int page = display / s_toc_page_rows;
        const int page_row = display % s_toc_page_rows;
        const bool home = s_toc_home && page_row == 0;
        const int i = s_toc_home ? page * (s_toc_page_rows - 1) + page_row - 1 : display;
        const auto* entry = home ? nullptr : session.TocAt(i);
        if (!home && !entry) {
            continue;
        }
        const char* title = home ? home_title : entry->title.c_str();
        std::string ordinal, name;
        const bool split = !home && SplitChapterHeading(entry->title, ordinal, name);
        const bool current = home ? at_home : !at_home && i == current_chapter;
        const auto* font = current ? UiFont(UiFontRole::LargeEmphasis) : Book_ItemFont();
        const int ordinal_w =
            split
                ? std::min(width / 2, std::max<int>(UiSx(90), ScreenMeasureTextWidth(font, ordinal.c_str()) + UiSx(12)))
                : 0;
        const int text_w = width - ordinal_w - UiSx(16);
        auto* row = s_toc_rows[slot];
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(row, display * s_toc_height);
        lv_obj_remove_event_cb(row, SheetTocJump);
        const int target = home ? kSheetProgressBase : i;
        lv_obj_add_event_cb(row, SheetTocJump, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(target)));
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_side(row, page_row == s_toc_page_rows - 1 ? LV_BORDER_SIDE_NONE : LV_BORDER_SIDE_BOTTOM,
                                     0);
        auto* text = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_font(text, font, 0);
        lv_obj_set_size(text, text_w, font->line_height);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_label_set_text(text, split ? name.c_str() : title);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(text, LV_ALIGN_LEFT_MID, ordinal_w + UiSx(8), 0);
        lv_obj_add_flag(lv_obj_get_child(row, 1), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_decor(text, LV_TEXT_DECOR_NONE, 0);
        if (split) {
            auto* number = lv_obj_get_child(row, 1);
            lv_obj_remove_flag(number, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(number, font, 0);
            lv_label_set_text(number, ordinal.c_str());
            lv_obj_set_size(number, ordinal_w, font->line_height);
            lv_label_set_long_mode(number, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(number, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }
    s_toc_binding = false;
}

enum BookSettingsSheetBookmarkAction {
    kBookmarkAdd,
    kBookmarkNext,
    kBookmarkCancel,
    kBookmarkAll,
    kBookmarkRemove,
    kBookmarkClick,
    kBookmarkLong,
    kBookmarkPrevious
};

struct BookSettingsSheetBookmarkRequest {
    BookSettingsSheetBookmarkAction action;
    uint32_t id;
    std::string path;
};

static lv_obj_t* s_bookmark_message = nullptr;

static void BookmarkMessage(const std::string& text) {
    if (StandbyPresentation::IsActive()) {
        return;
    }
    CloseBookmarkMessage();
    auto* box = lv_msgbox_create(nullptr);
    s_bookmark_message = box;
    lv_obj_add_event_cb(
        box,
        [](lv_event_t* event) {
            if (lv_event_get_target(event) == s_bookmark_message) {
                s_bookmark_message = nullptr;
            }
        },
        LV_EVENT_DELETE, nullptr);
    auto* title = lv_msgbox_add_title(box, LangString_BOOK_BOOKMARKS);
    auto* label = lv_msgbox_add_text(box, text.c_str());
    lv_obj_set_style_text_font(title, Book_ListFont(), 0);
    lv_obj_set_style_text_font(label, Book_ItemFont(), 0);
    lv_msgbox_add_close_button(box);
}

struct BookBookmarkUiResult : ScreenUiCompletion {
    BookInfo info;
    BookBookmarkRequest request;
};
static BookBookmarkUiResult* s_bookmark_write = nullptr; // 完成交付前不接纳第二次修改

static void ApplyBookmarkSave(ScreenUiCompletion* completion) {
    auto* result = static_cast<BookBookmarkUiResult*>(completion);
    auto& state = Book_State();
    auto* session = state.reader.session.get();
    if (state.reader.opening.load()) {
        ScreenCompleteWork(result, true);
        return;
    }
    std::unique_ptr<BookBookmarkUiResult> owner(result);
    s_bookmark_write = nullptr;
    if (!session || !session->IsOpen() || session->Info().path != result->info.path ||
        session->Info().file_size != result->info.file_size || session->Info().mtime != result->info.mtime) {
        return;
    }
    if (result->request->ok) {
        session->AdoptBookmarks(std::move(result->request->store));
        if (state.reader.content && !state.reader.standby_paused) {
            ResetBookmarkSelection();
            RequestSettingsSheetPaint();
        }
    } else if (state.reader.content && !state.reader.standby_paused) {
        BookmarkMessage(result->request->error.empty() ? LangString_COMMON_MEMORY_RETRY
                                                       : result->request->error.c_str());
    }
}

static bool SaveBookmarkChange(BookSession& session, std::vector<uint32_t> remove) {
    if (s_bookmark_write) {
        BookmarkMessage(LangString_BOOK_LAYOUT_WAIT);
        return false;
    }
    BookBookmark entry;
    std::string error;
    if (remove.empty() && !session.CaptureCurrentPageBookmark(entry, error)) {
        BookmarkMessage(error);
        return false;
    }
    auto result = std::make_unique<BookBookmarkUiResult>();
    result->info = session.Info();
    result->apply = ApplyBookmarkSave;
    auto* context = result.get();
    result->request = BookBookmarkSubmit(session.BookmarkStore(), std::move(entry), std::move(remove),
                                         [context]() { ScreenCompleteWork(context, true); });
    if (!result->request) {
        BookmarkMessage(LangString_COMMON_MEMORY_RETRY);
        return false;
    }
    s_bookmark_write = result.release();
    return true;
}

static void BookmarkRequestAsync(void* data) {
    std::unique_ptr<BookSettingsSheetBookmarkRequest> request(static_cast<BookSettingsSheetBookmarkRequest*>(data));
    auto& st = Book_State();
    auto* session = st.reader.session.get();
    if (StandbyPresentation::IsActive() || !session || !session->IsOpen() || session->Info().path != request->path ||
        st.reader.read_chrome != BookUiState::ReadChrome::kSettings) {
        return;
    }
    try {
        auto& selected = st.settings.bookmark_selected;
        if (request->action == kBookmarkCancel) {
            ResetBookmarkSelection();
        } else if (request->action == kBookmarkNext || request->action == kBookmarkPrevious) {
            const int groups = std::max<int>(1, (session->BookmarkStore().Entries().size() + 5) / 6);
            const int delta = request->action == kBookmarkNext ? 1 : -1;
            st.settings.bookmark_group = std::max(0, std::min(groups - 1, st.settings.bookmark_group + delta));
            st.settings.reader_scroll[2] = 0;
        } else if (request->action == kBookmarkAll) {
            selected.clear();
            for (const auto& entry : session->BookmarkStore().Entries()) {
                selected.push_back(entry.id);
            }
        } else if (request->action == kBookmarkLong ||
                   (request->action == kBookmarkClick && st.settings.bookmark_multi)) {
            const auto& entries = session->BookmarkStore().Entries();
            if (std::none_of(entries.begin(), entries.end(),
                             [&](const BookBookmark& entry) { return entry.id == request->id; })) {
                return;
            }
            const auto found = std::find(selected.begin(), selected.end(), request->id);
            if (request->action == kBookmarkLong) {
                st.settings.bookmark_multi = true;
                if (found == selected.end()) {
                    selected.push_back(request->id);
                }
            } else if (found == selected.end()) {
                selected.push_back(request->id);
            } else {
                selected.erase(found);
            }
        } else {
            if (!SheetNavigationReady()) {
                BookmarkMessage(LangString_BOOK_LAYOUT_WAIT);
                return;
            }
            if (request->action == kBookmarkAdd) {
                const uint32_t current = session->CurrentPageBookmarkId();
                if (current) {
                    const std::string path = session->Info().path;
                    ScreenConfirmDelete(lv_screen_active(), [path, current] {
                        auto* active = Book_State().reader.session.get();
                        if (active && active->Info().path == path) {
                            SaveBookmarkChange(*active, {current});
                        }
                    });
                } else {
                    SaveBookmarkChange(*session, {});
                }
                return;
            } else if (request->action == kBookmarkRemove) {
                if (selected.empty()) {
                    return;
                }
                SaveBookmarkChange(*session, selected);
                return;
            } else if (request->action == kBookmarkClick) {
                if (!RequestReaderNavigation(BookSession::NavigationKind::kBookmark, request->id)) {
                    BookmarkMessage(LangString_BOOK_BOOKMARK_LOCATE_FAILED);
                    return;
                }
                HideReadChrome();
                RequestRenderReaderPage();
                return;
            }
        }
        RequestSettingsSheetPaint();
    } catch (const std::bad_alloc&) {
        BookmarkMessage(LangString_COMMON_MEMORY_RETRY);
    }
}

static void QueueBookmarkAction(BookSettingsSheetBookmarkAction action, uint32_t id = 0) {
    auto& st = Book_State();
    if (!st.reader.session) {
        return;
    }
    try {
        auto* request = new (std::nothrow) BookSettingsSheetBookmarkRequest{action, id, st.reader.session->Info().path};
        if (request && !BookLvAsync(BookmarkRequestAsync, request)) {
            delete request;
        }
    } catch (const std::bad_alloc&) {
        BookmarkMessage(LangString_COMMON_MEMORY_RETRY);
    }
}

static void BookmarkActionEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const auto action =
        static_cast<BookSettingsSheetBookmarkAction>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (action == kBookmarkRemove) {
        if (!Book_State().settings.bookmark_selected.empty()) {
            ScreenConfirmDelete(lv_screen_active(), [] { QueueBookmarkAction(kBookmarkRemove); });
        }
    } else {
        QueueBookmarkAction(action);
    }
}

static void BookmarkContentEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    QueueBookmarkAction(lv_event_get_code(event) == LV_EVENT_LONG_PRESSED ? kBookmarkLong : kBookmarkClick,
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

static void BindBookmarkContent(lv_obj_t* object, uint32_t id) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(object);
    // SHORT_CLICKED never follows LONG_PRESSED, so release cannot undo selection.
    lv_obj_add_event_cb(object, BookmarkContentEvent, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    lv_obj_add_event_cb(object, BookmarkContentEvent, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
}

static void BuildSheetBookmarks(lv_obj_t* body) {
    auto& st = Book_State();
    auto& session = *st.reader.session;
    const auto& entries = session.BookmarkStore().Entries();
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body);
    const bool landscape = LV_HOR_RES > LV_VER_RES;
    const int height = lv_obj_get_content_height(body);
    auto* page = SheetBox(body, width, height);
    const auto x = [width, landscape](int value) { return value * (landscape ? (width - 16) / 2 : width) / 701; };
    const auto y = [height](int value) { return value * height / 1245; };
    const lv_font_t* font = Book_ItemFont();
    const int footer_h = landscape ? font->line_height + 28 : 0;
    const int content_h = height - footer_h;
    const int column_w = (width - 16) / 2;
    const auto add_art_pixels = [&](const lv_image_dsc_t* src, int ax, int ay, int aw, int ah) {
        auto* slot = SheetBox(page, aw, ah);
        lv_obj_set_pos(slot, ax, ay);
        auto* image = lv_image_create(slot);
        lv_image_set_src(image, src);
        const int scale = std::min(aw * 256 / src->header.w, ah * 256 / src->header.h);
        lv_image_set_scale(image, std::max(1, scale));
        lv_obj_center(image);
        lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    };
    const auto add_art = [&](const lv_image_dsc_t* src, int ax, int ay, int aw, int ah) {
        add_art_pixels(src, x(ax), y(ay), x(aw), y(ah));
    };
    if (landscape) {
        const int art_h = content_h * 3 / 5, art_w = art_h * 3 / 8;
        add_art_pixels(BookmarkTitleImage(), column_w - art_w - 14, (content_h - art_h) / 2, art_w, art_h);
        auto* divider = SheetBox(page, 2, content_h);
        lv_obj_set_pos(divider, width / 2 - 1, 0);
        lv_obj_set_style_bg_color(divider, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    } else {
        add_art(BookmarkTitleImage(), 570, 236, 90, 240);
    }
    bool fill = false;
    const int ch = landscape ? content_h * 7 / 10 : y(332), cw = landscape ? ch * 232 / 332 : x(232);
    // Bound a -10 degree rotation around the cover centre, with rounding slack.
    const int frame_w = static_cast<int>(std::ceil(cw * 0.984807753 + ch * 0.173648178)) + 8;
    const int frame_h = static_cast<int>(std::ceil(ch * 0.984807753 + cw * 0.173648178)) + 8;
    auto* cover_frame = SheetBox(page, frame_w, frame_h);
    const int cover_x = landscape ? (width + 16) / 2 + 12 : x(40) + (x(260) - frame_w) / 2;
    const int cover_y = landscape ? (content_h - frame_h) / 2 : y(740) + (y(326) - frame_h) / 2;
    lv_obj_set_pos(cover_frame, cover_x, cover_y);
    auto* cover = CreateCoverSlot(cover_frame, session.Info(), cw, ch, &st.settings.reader_sheet_cover, &fill);
    lv_obj_center(cover);
    lv_obj_set_style_transform_pivot_x(cover, cw / 2, 0);
    lv_obj_set_style_transform_pivot_y(cover, ch / 2, 0);
    lv_obj_set_style_transform_rotation(cover, -100, 0);
    if (fill) {
        EnqueueListCoverFill(session.Info(), CoverFrameInner(cw), CoverFrameInner(ch), cover,
                             &st.settings.reader_sheet_cover);
        ScheduleListCoverFill();
    } else if (st.settings.reader_sheet_cover.empty()) {
        auto* name = lv_obj_get_child_by_type(cover, 0, &lv_label_class);
        if (!name) {
            ESP_LOGE("BookScreen", "bookmark cover placeholder label missing");
            return;
        }
        lv_label_set_text(name, session.Title().c_str());
        lv_obj_set_width(name, cw - UiSx(12));
        lv_obj_set_height(name, Book_ItemFont()->line_height * 4);
        lv_obj_set_style_text_font(name, Book_ItemFont(), 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_center(name);
    }
    if (landscape) {
        add_art_pixels(BookmarkClipImage(), cover_x + frame_w * 3 / 5, cover_y - 8, 36, 36);
    } else {
        add_art(BookmarkClipImage(), 200, 704, 66, 66);
    }

    struct Sample {
        int x, width, dot_x;
    };

    const Sample samples[] = {
        {112, 294, 462}, {112, 294, 462}, {112, 294, 462}, {398, 270, 354}, {398, 270, 354}, {398, 270, 354},
    };
    // Equal-height excerpt slots and a fixed row stride prevent long text from
    // crowding the next bookmark. Keep three lines at most, with ellipsis.
    const auto* excerpt_font = UiFont(UiFontRole::Emphasis);
    const int line_height = excerpt_font->line_height + 2;
    const int text_h = 3 * line_height - 2;
    const int row_step = landscape ? std::max(text_h + 24, (content_h - 16) / 3) : std::max(y(180), text_h + y(24) + 6);
    if (!landscape) {
        lv_obj_set_height(page, std::max(height, y(70) + 5 * row_step + text_h + y(80)));
    }
    if (entries.empty()) {
        auto* empty =
            SheetLabel(page, session.BookmarkStore().Error().empty() ? LangString_BOOK_BOOKMARKS_EMPTY
                                                                     : session.BookmarkStore().Error().c_str());
        lv_obj_set_width(empty, landscape ? column_w - 24 : width * 3 / 5);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        if (landscape) {
            lv_obj_update_layout(empty);
            lv_obj_set_pos(empty, 12, std::max<int>(0, (content_h - lv_obj_get_height(empty)) / 2));
        } else {
            lv_obj_set_pos(empty, x(110), y(120));
        }
    }
    const int groups = std::max<int>(1, (entries.size() + 5) / 6);
    st.settings.bookmark_group = std::min(st.settings.bookmark_group, groups - 1);
    for (int i = 0; i < 6; ++i) {
        const size_t index = st.settings.bookmark_group * 6 + i;
        if (index >= entries.size()) {
            break;
        }
        const auto& entry = entries[index];
        const auto& sample = samples[i];
        const int row_top = landscape ? 8 + (i % 3) * row_step : y(70) + i * row_step;
        const int column_x = landscape && i >= 3 ? (width + 16) / 2 : 0;
        // All three excerpts in each group share the same text and dot columns.
        // Extend away from the dot/decorations so the existing separation stays intact.
        const int extra_width =
            i < 3 ? x(40) : std::max(0, std::min(x(40), width - column_x - x(sample.x) - x(sample.width) - 8));
        const int text_x = column_x + x(sample.x) - (i < 3 ? extra_width : 0);
        const int text_w = x(sample.width) + extra_width;
        auto* label = SheetLabel(page, entry.excerpt.c_str(), excerpt_font);
        BindBookmarkContent(label, entry.id);
        lv_obj_set_size(label, text_w, text_h);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 2, 0);
        lv_obj_set_pos(label, text_x, row_top);
        auto* line = SheetBox(page, text_w, 2);
        lv_obj_set_pos(line, text_x, row_top + text_h + 4);
        lv_obj_set_style_bg_color(line, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        const int dot_size = st.settings.bookmark_multi ? std::max(22, x(24)) : std::max(10, x(18));
        auto* dot = SheetBox(page, dot_size, dot_size);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, st.settings.bookmark_multi ? UiColorBackground() : UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, column_x + x(sample.dot_x) - dot_size / 2, row_top + text_h / 2 - dot_size / 2);
        BindBookmarkContent(dot, entry.id);
        if (st.settings.bookmark_multi) {
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_border_color(dot, UiColorForeground(), 0);
            if (std::find(st.settings.bookmark_selected.begin(), st.settings.bookmark_selected.end(), entry.id) !=
                st.settings.bookmark_selected.end()) {
                static const lv_point_precise_t points[] = {{0, 5}, {4, 9}, {11, 0}};
                auto* check = lv_line_create(dot);
                lv_line_set_points(check, points, 3);
                lv_obj_set_style_line_width(check, 2, 0);
                lv_obj_set_style_line_color(check, UiColorForeground(), 0);
                lv_obj_center(check);
                lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
    if (st.settings.bookmark_group > 0) {
        auto* previous = SheetButton(page, "<<", x(64), std::max<int>(UiSy(30), font->line_height + 8),
                                     BookmarkActionEvent, kBookmarkPrevious);
        lv_obj_set_style_border_width(previous, 0, 0);
        lv_obj_align(previous, LV_ALIGN_BOTTOM_LEFT, x(16), landscape ? -8 : -y(18));
    }
    if (st.settings.bookmark_group + 1 < groups) {
        auto* more = SheetButton(page, ">>", x(64), std::max<int>(UiSy(30), font->line_height + 8), BookmarkActionEvent,
                                 kBookmarkNext);
        lv_obj_set_style_border_width(more, 0, 0);
        lv_obj_align(more, LV_ALIGN_BOTTOM_RIGHT, -x(16), landscape ? -8 : -y(18));
    }
    if (groups > 1) {
        char text[24];
        std::snprintf(text, sizeof(text), "%d / %d", st.settings.bookmark_group + 1, groups);
        auto* count = SheetLabel(page, text, font);
        lv_obj_align(count, LV_ALIGN_BOTTOM_MID, 0, landscape ? -12 : -y(20));
    }
}

static void BuildSheetDuration(lv_obj_t* body) {
    auto& session = *Book_State().reader.session;
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body);
    auto* bookmark = SheetButton(body, "", UiSx(34), UiSy(34), BookmarkActionEvent, kBookmarkAdd,
                                 session.CurrentPageBookmarkId() != 0);
    lv_obj_add_flag(bookmark, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(bookmark, LV_ALIGN_TOP_RIGHT, -UiSx(4), 6);
    lv_obj_set_style_radius(bookmark, LV_RADIUS_CIRCLE, 0);
    s_bookmark_button = bookmark;
    s_bookmark_icon = lv_image_create(bookmark);
    lv_image_set_src(s_bookmark_icon, SheetBookmarkIcon());
    lv_obj_set_style_image_recolor_opa(s_bookmark_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(s_bookmark_icon,
                                   session.CurrentPageBookmarkId() ? UiColorBackground() : UiColorForeground(), 0);
    lv_obj_center(s_bookmark_icon);
    lv_obj_clear_flag(s_bookmark_icon, LV_OBJ_FLAG_CLICKABLE);
    const int summary_h = ReadingSummaryHeight();
    auto* row = SheetBox(body, width, summary_h);
    const uint32_t seconds = session.ReadingSeconds();
    char duration[64];
    std::snprintf(duration, sizeof(duration), LangString_BOOK_DURATION_HM_FMT, seconds / 3600, seconds % 3600 / 60);
    char progress[64];
    const int pct = session.ReadingProgressX10();
    std::snprintf(progress, sizeof(progress), "%.2f%%", session.ReadingProgressX10() / 10.0);
    const char* titles[] = {LangString_BOOK_READ_DURATION, LangString_BOOK_READING_PROGRESS};
    const char* values[] = {duration, progress};
    for (int i = 0; i < 2; ++i) {
        auto* column = SheetBox(row, width / 2, summary_h);
        lv_obj_set_pos(column, i * width / 2, 0);
        auto* title = SheetLabel(column, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UiSy(22));
        auto* value = SheetLabel(column, values[i], Book_ItemFont());
        if (i == 0) {
            s_duration_value = value;
        } else {
            s_progress_value = value;
        }
        lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -UiSy(5));
    }
    auto* line = SheetBox(row, 1, UiSy(44));
    lv_obj_set_style_bg_color(line, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_center(line);
    const int controls_h = ReadingProgressControlsHeight();
    auto* controls = SheetBox(body, width, controls_h);
    const bool comic = session.IsPageImagesMode();
    const char* previous_title = comic ? LangString_BOOK_PAGE_PREV : LangString_BOOK_CHAPTER_PREV;
    const char* next_title = comic ? LangString_BOOK_PAGE_NEXT : LangString_BOOK_CHAPTER_NEXT;
    lv_point_t previous_size, next_size;
    lv_text_get_size(&previous_size, previous_title, Book_ItemFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&next_size, next_title, Book_ItemFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int chapter_w = std::min<int>(width / 3, std::max(previous_size.x, next_size.x) + 16);
    auto* prev = SheetButton(controls, previous_title, chapter_w, controls_h - UiSy(8), SheetChapter, -1);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 0);
    auto* next = SheetButton(controls, next_title, chapter_w, controls_h - UiSy(8), SheetChapter, 1);
    s_chapter_prev = prev;
    s_chapter_next = next;
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_border_width(prev, 0, 0);
    lv_obj_set_style_border_width(next, 0, 0);
    auto* slider = SheetSlider(controls, pct, 1000, width - 2 * chapter_w - UiSx(24));
    lv_obj_center(slider);
    s_progress_slider = slider;
    lv_obj_add_event_cb(slider, SheetSeek, LV_EVENT_RELEASED, nullptr);
    const bool ready = SheetNavigationReady();
    SetSheetBtnEnabled(prev, ready && (comic ? session.HasPrevPage() : session.CurrentTocIndex() > 0));
    SetSheetBtnEnabled(next,
                       ready && (comic ? session.HasNextPage() : session.CurrentTocIndex() + 1 < session.TocCount()));
    if (!ready) {
        lv_obj_add_state(slider, LV_STATE_DISABLED);
    }
}

static void SetLiveSheetButton(lv_obj_t* button, bool active) {
    const bool was_active = lv_color_eq(lv_obj_get_style_bg_color(button, LV_PART_MAIN), UiColorForeground());
    if (was_active == active) {
        return;
    }
    lv_obj_set_style_bg_color(button, active ? UiColorForeground() : UiColorBackground(), 0);
    auto* label = lv_obj_get_child(button, 0);
    if (label) {
        ScreenFitLabel(label, lv_obj_get_width(label), lv_obj_get_height(label),
                       active ? UiFont(UiFontRole::Emphasis) : Book_ItemFont());
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, active ? UiColorBackground() : UiColorForeground(), 0);
    }
    if (active) {
        EpdRequestBlackEqualization();
    }
}

void BuildReadingBottomSheet() {
    auto& st = Book_State();
    auto* sheet = st.settings.settings_sheet;
    if (!sheet || !lv_obj_is_valid(sheet) || !st.reader.session) {
        return;
    }
    FlashCacheGuard display_guard;
    if (st.settings.bookmark_ui_path != st.reader.session->Info().path) {
        ResetBookmarkSelection();
        st.settings.bookmark_group = 0;
        st.settings.reader_scroll[2] = 0;
        st.settings.bookmark_ui_path = st.reader.session->Info().path;
    }
    CancelListCoverFill();
    s_settings_body_height = 0;
    ClearSettingsSheetWidgetRefs();
    lv_obj_clean(sheet);
    s_sheet_bindings.reserve(40);
    st.settings.reader_sheet_cover.Reset();
    lv_obj_remove_style_all(sheet);
    lv_obj_set_style_bg_color(sheet, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_border_width(sheet, kReadingCardBorderWidth, 0);
    lv_obj_set_style_border_color(sheet, UiColorForeground(), 0);
    lv_obj_set_style_border_post(sheet, true, 0);
    Book_DisableScroll(sheet);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ResizeReadingBottomSheet();
    lv_obj_update_layout(sheet);
    const int sheet_width = lv_obj_get_content_width(sheet);
    const bool duration = st.settings.reader_tab == kDurationTab;
    const int header_h = ReadingSheetTabHeaderHeight();
    const int nav_h = ReadingSheetNavReserve();
    if (!duration) {
        auto* header = SheetBox(sheet, sheet_width, header_h);
        const char* titles[] = {LangString_BOOK_TOC, LangString_BOOK_FONT_SELECT, LangString_BOOK_BOOKMARKS,
                                LangString_BOOK_READING_SETTINGS};
        char font_selection[32];
        const bool tap_pick =
            st.settings.reader_tab == 3 && st.settings.reader_settings_page == kSettingsTapZones && TapZonePickOpen();
        const char* header_title = tap_pick ? LangString_BOOK_TAP_ZONES_PICK
                                   : st.settings.reader_tab == 3
                                       ? SettingsPageTitle(st.settings.reader_settings_page)
                                       : titles[st.settings.reader_tab];
        if (st.settings.reader_tab == 1 && st.settings.font_multi) {
            std::snprintf(font_selection, sizeof(font_selection), LangString_BOOK_SELECTED_FMT, FontSelectedCount());
            header_title = font_selection;
        }
        lv_event_cb_t reset = nullptr;
        if ((st.settings.reader_tab == 1 && !st.settings.font_multi) ||
            (st.settings.reader_tab == 3 && st.settings.reader_settings_page == kSettingsHome)) {
            reset = SettingsReset;
        } else if (st.settings.reader_tab == 3 && st.settings.reader_settings_page == kSettingsTapZones && !tap_pick) {
            reset = OnTapZoneReset;
        }
        BuildReadingSheetHeader(header, header_title, SettingsBack,
                                st.settings.reader_tab == 3 && st.settings.reader_settings_page != kSettingsHome, reset);
        if (st.settings.reader_tab == 2 && st.settings.bookmark_multi) {
            char count[32];
            std::snprintf(count, sizeof(count), LangString_BOOK_SELECTED_ITEMS_FMT,
                          static_cast<unsigned>(st.settings.bookmark_selected.size()));
            auto* label = SheetLabel(header, count, Book_ListFont());
            lv_obj_set_style_bg_color(label, UiColorBackground(), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
            lv_obj_center(label);
        }
    }
    lv_obj_update_layout(sheet);
    auto* body = SheetBox(sheet, sheet_width, std::max<int>(1, lv_obj_get_content_height(sheet) - header_h - nav_h));
    st.settings.reader_sheet_body = body;
    lv_obj_set_pos(body, 0, header_h);
    lv_obj_set_style_pad_hor(body, UiSx(16), 0);
    lv_obj_set_style_pad_ver(body, ReadingSheetBodyPadding(), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 0, 0);
    if (st.settings.reader_tab == kDurationTab) {
        ConfigureSheetScroll(body);
    }
    if (duration) {
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    }
    lv_obj_update_layout(body);
    if (st.settings.reader_tab == 0) {
        BuildSheetToc(body);
    } else if (st.settings.reader_tab == 1) {
        BuildSheetFonts(body);
    } else if (st.settings.reader_tab == kDurationTab) {
        BuildSheetDuration(body);
    } else if (st.settings.reader_tab == 2) {
        BuildSheetBookmarks(body);
    } else {
        BuildSheetSettings(body);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_update_layout(body);
        s_settings_body_height = lv_obj_get_height(body);
        ResizeReadingBottomSheet();
        lv_obj_update_layout(sheet);
        const int available = std::max<int>(1, lv_obj_get_content_height(sheet) - header_h - nav_h);
        lv_obj_set_height(body, available);
        if (s_settings_body_height <= available) {
            Book_DisableScroll(body);
            lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
        }
    }
    auto* nav = SheetBox(sheet, sheet_width, nav_h);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(nav, kReadingCardBorderWidth, 0);
    lv_obj_set_style_border_color(nav, UiColorForeground(), 0);
    const char* tabs[] = {LangString_BOOK_TOC, LangString_BOOK_FONT_TITLE, LangString_BOOK_BOOKMARKS,
                          LangString_HOME_APP_SETTINGS};
    const lv_image_dsc_t* icons[] = {SheetFontIcon(), SheetTocIcon(), SheetBookmarkNavIcon(), SheetSettingsIcon()};
    const int bottom_gap =
        LV_HOR_RES > LV_VER_RES ? 8 : UiSy(10);  // Additional space inside the display's configured 8px content inset.
    if (st.settings.reader_tab == 2 && st.settings.bookmark_multi) {
        const char* actions[] = {LangString_COMMON_CANCEL, LangString_COMMON_SELECT_ALL, LangString_COMMON_REMOVE};
        const BookSettingsSheetBookmarkAction codes[] = {kBookmarkCancel, kBookmarkAll, kBookmarkRemove};
        for (int i = 0; i < 3; ++i) {
            auto* button = SheetButton(nav, actions[i], sheet_width / 3 - UiSx(16),
                                       std::min<int>(UiSy(34), nav_h - bottom_gap - 6), BookmarkActionEvent, codes[i]);
            lv_obj_set_pos(button, sheet_width * i / 3 + UiSx(8), UiSy(16));
            if (i == 2) {
                SetSheetBtnEnabled(button, !st.settings.bookmark_selected.empty());
            }
        }
    } else if (st.settings.reader_tab == 1 && (st.settings.font_multi || st.ttf.ttf_multi)) {
        const bool ttf_multi = st.ttf.ttf_multi;
        const char* actions[] = {LangString_COMMON_CANCEL, LangString_COMMON_SELECT_ALL, LangString_COMMON_REMOVE};
        const lv_event_cb_t callbacks[] = {ttf_multi ? OnTtfMultiCancel : OnFontMultiCancel,
                                           ttf_multi ? OnTtfMultiSelectAll : OnFontMultiSelectAll,
                                           ttf_multi ? OnTtfMultiRemove : OnFontMultiRemove};
        for (int i = 0; i < 3; ++i) {
            auto* button = SheetButton(nav, actions[i], sheet_width / 3 - UiSx(16),
                                       std::min<int>(UiSy(34), nav_h - bottom_gap - 6), callbacks[i]);
            lv_obj_set_pos(button, sheet_width * i / 3 + UiSx(8), UiSy(16));
            if (i == 2) {
                SetSheetBtnEnabled(button, (ttf_multi ? TtfSelectedCount() : FontSelectedCount()) > 0 &&
                                               (ttf_multi || !ReaderWorkersBusy()));
            }
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            auto* btn = SheetBox(nav, sheet_width / 4, nav_h - bottom_gap - 2);
            lv_obj_set_pos(btn, i * sheet_width / 4, 1);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(btn);
            lv_obj_add_event_cb(btn, SheetTabEvent, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            const bool landscape = LV_HOR_RES > LV_VER_RES;
            const int padding = landscape ? 4 : UiSy(8);
            const int label_height =
                std::max<int>(Book_ItemFont()->line_height, UiFont(UiFontRole::TitleEmphasis)->line_height);
            const int icon_edge =
                std::max(1, std::min<int>({32, sheet_width / 4 - 2 * padding,
                                           nav_h - bottom_gap - 2 - label_height - UiSy(4) - 2 * padding}));
            auto* icon = lv_image_create(btn);
            lv_image_set_src(icon, icons[i]);
            const int scale = std::min(256, icon_edge * 256 / 32);
            lv_image_set_scale(icon, std::max(1, scale));
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, padding);
            auto* label = SheetLabel(
                btn, tabs[i], UiFont(st.settings.reader_tab == i ? UiFontRole::TitleEmphasis : UiFontRole::Body));
            ScreenFitLabel(label, sheet_width / 4 - UiSx(8), label_height,
                           lv_obj_get_style_text_font(label, LV_PART_MAIN));
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -padding);
            if (st.settings.reader_tab == i) {
                lv_obj_update_layout(btn);
                const int mark_width = std::min<int>(UiSx(44), lv_obj_get_content_width(btn));
                auto* mark = SheetBox(btn, mark_width, UiSy(2));
                lv_obj_set_style_bg_color(mark, UiColorForeground(), 0);
                lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
                lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
        }
    }
    lv_obj_update_layout(body);
    if (st.settings.reader_sheet_scroll) {
        int offset = st.settings.reader_scroll[st.settings.reader_tab];
        const auto& pages = st.settings.reader_tab == 1 ? s_font_pages : s_toc_pages;
        if (pages.list == st.settings.reader_sheet_scroll) {
            offset = std::clamp(std::max(0, offset) / pages.stride, 0, pages.pages - 1) * pages.stride;
        }
        lv_obj_scroll_to_y(st.settings.reader_sheet_scroll, offset, LV_ANIM_OFF);
        if (st.settings.reader_tab == 0 || st.settings.reader_tab == 1) {
            lv_obj_send_event(st.settings.reader_sheet_scroll, LV_EVENT_SCROLL, nullptr);
        }
    }
    s_live_body = body;
    s_live_tab = st.settings.reader_tab;
    s_live_width = LV_HOR_RES;
    s_live_height = LV_VER_RES;
    s_live_settings_page = st.settings.reader_settings_page;
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) {
        TtfEnsurePanel();
    }
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) {
        SetReadStatusVisible(true);
    }
}

bool DeferComicTextSetting() {
    if (!ReaderPageImagesHint()) {
        return false;
    }
    ScreenShowToast(LangString_BOOK_COMIC_SETTING_SAVED);
    return true;
}

void RebuildSettingsSheet() {
    if (Book_State().reader.standby_paused) {
        return;
    }
    BuildReadingBottomSheet();
}

void ResizeReadingBottomSheet() {
    auto& st = Book_State();
    auto* sheet = st.settings.settings_sheet;
    if (!sheet || !lv_obj_is_valid(sheet)) {
        return;
    }
    const bool compact = st.settings.reader_tab == kDurationTab;
    const bool full = st.settings.reader_tab == 0 || st.settings.reader_tab == 2;
    const int header_h = ReadingSheetTabHeaderHeight();
    const int nav_h = ReadingSheetNavReserve();
    int desired = compact ? ReadingSummaryHeight() + ReadingProgressControlsHeight() + 2 * ReadingSheetBodyPadding() +
                                nav_h + 2 * kReadingCardBorderWidth
                  : full  ? LV_VER_RES
                  : st.settings.reader_tab == 3
                      ? header_h + (s_settings_body_height > 0 ? s_settings_body_height : LV_VER_RES) + nav_h +
                            2 * kReadingCardBorderWidth
                      : UiSy(430);
    if (st.settings.reader_tab == 1) {
        desired = std::max<int>(UiSy(440), header_h + FontGridBodyHeight() + nav_h + 2 * kReadingCardBorderWidth);
    }
    const int limit = compact ? LV_VER_RES * 3 / 5
                      : full || st.settings.reader_tab == 1
                          ? LV_VER_RES
                          : LV_VER_RES - (st.reader.status_h > 0 ? st.reader.status_h : 40);
    const int status_h = st.reader.status_h > 0 ? st.reader.status_h : 40;
    const int available = std::max<int>(1, LV_VER_RES - status_h);
    const int height = std::min<int>(desired, std::min(limit, available));
    lv_obj_set_size(sheet, std::max<int>(1, LV_HOR_RES - 2 * kReadingCardSideInset), height);
    lv_obj_set_style_border_side(sheet, height == available ? LV_BORDER_SIDE_NONE : LV_BORDER_SIDE_TOP, 0);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void SelectReadingSheetTab(int tab) {
    if (tab < 0 || tab > 3) {
        return;
    }
    const auto& settings = Book_State().settings;
    if (tab != 1 && !(tab == 3 && settings.reader_settings_page != kSettingsHome) && tab == settings.reader_tab &&
        s_live_tab == tab && s_live_body && s_live_body == settings.reader_sheet_body && lv_obj_is_valid(s_live_body) &&
        s_live_width == LV_HOR_RES && s_live_height == LV_VER_RES) {
        return;
    }
    if (tab != 2) {
        ResetBookmarkSelection();
    }
    TtfPanelClose();
    Book_State().tap.tap_zone_pick_cell = -1;
    Book_State().settings.reader_tab = tab;
    if (tab == 3) {
        Book_State().settings.reader_settings_page = kSettingsHome;
    }
    if (tab == 1) {
        StartReadFontScan(true);
    } else {
        CancelReadFontScan();
    }
    if (tab == 0 || tab == 1) {
        Book_State().settings.reader_scroll[tab] = -1;
    }
    RebuildSettingsSheet();
}

bool CancelBookmarkMultiSelection() {
    if (!Book_State().settings.bookmark_multi) {
        return false;
    }
    ResetBookmarkSelection();
    RequestSettingsSheetPaint();
    return true;
}

void ResetBookmarkSelection() {
    Book_State().settings.bookmark_multi = false;
    Book_State().settings.bookmark_selected.clear();
}

bool BookmarkMessageVisible() {
    return s_bookmark_message != nullptr;
}

bool CloseBookmarkMessage() {
    if (!s_bookmark_message) {
        return false;
    }
    auto* box = s_bookmark_message;
    s_bookmark_message = nullptr;
    lv_msgbox_close(box);
    return true;
}

lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSheetIconBtn, kSheetIconBtn);
    lv_obj_set_style_bg_color(btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kSheetBorderW, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_radius(btn, kSheetIconBtn / 2, 0);
    Book_DisableScroll(btn);
    // 始终挂事件，原地 SetSheetBtnEnabled 切换可点态（勿依赖重建）
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    SetSheetBtnEnabled(btn, enabled);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, Book_ListFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

lv_obj_t* MakeSheetTextLink(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, kSheetIconBtn);
    lv_obj_set_style_pad_hor(btn, 4, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    Book_DisableScroll(btn);

    lv_obj_t* text_wrap = lv_obj_create(btn);
    lv_obj_remove_style_all(text_wrap);
    lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(text_wrap, 2, 0);
    lv_obj_set_style_border_color(text_wrap, UiColorForeground(), 0);
    lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
    lv_obj_set_style_pad_hor(text_wrap, 5, 0);
    lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
    Book_DisableScroll(text_wrap);
    lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(text_wrap);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void ApplyInformationGeometry(const lv_area_t& previous_area) {
    RequestSettingsSheetPaint();
    SyncFooterClockTimer();
    const auto area = ReadContentArea();
    if (area.y1 != previous_area.y1 || area.y2 != previous_area.y2) {
        if (Book_State().reader.layout_debounce_timer != nullptr) {
            FlushLayoutDebounce();
        } else {
            ApplyReaderLayoutLive(false);
        }
        return;
    }
    ApplyReaderChromeSize();
    UpdateReadFooter();
}

static void OnSheetFooterLayout(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const int layout = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (layout == BookReaderPrefsFooterLayout()) {
        return;
    }
    const auto area = ReadContentArea();
    BookReaderPrefsSetFooterLayout(layout);
    ApplyInformationGeometry(area);
}

void OnSheetFooterBitToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int bit = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const auto area = ReadContentArea();
    BookReaderPrefsToggleFooterBit(bit);
    ApplyInformationGeometry(area);
}

void OnSheetUnderlineMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        return;
    }
    const int current = BookReaderPrefsUnderlineMode();
    const int next = mode;
    if (next == current) {
        return;
    }
    BookReaderPrefsSetUnderlineMode(next);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) {
        return;
    }
    CancelReaderPageWork();
    RequestRenderReaderPage();
}

void OnSheetOrientMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode < kBookReaderOrientAuto || mode >= kBookReaderOrientCount) {
        return;
    }
    if (BookReaderPrefsOrient() == mode) {
        return;
    }
    BookReaderPrefsSetOrient(mode);
    SyncReaderAutoOrient();
    if (mode == kBookReaderOrientAuto || mode == DisplayUiGetOrient()) {
        RequestSettingsSheetPaint();
    } else {
        ApplyReaderOrient(mode);
    }
}

int ReadingSheetNavReserve() {
    return SheetNavHeight() + 4;
}

bool RefreshReadingSheetInPlace() {
    auto& st = Book_State();
    if (!s_live_body || s_live_body != st.settings.reader_sheet_body || !lv_obj_is_valid(s_live_body) ||
        s_live_tab != st.settings.reader_tab || s_live_width != LV_HOR_RES || s_live_height != LV_VER_RES) {
        return false;
    }
    if (s_live_tab == 3 && s_live_settings_page != st.settings.reader_settings_page) {
        return false;
    }
    if (NumericAdjustmentActive()) {
        s_numeric_refresh_pending = true;
        return true;
    }
    if (s_live_tab == 1) {
        return RefreshSheetFontControls();
    }
    if (s_live_tab != 0 && s_live_tab != 3 && s_live_tab != kDurationTab) {
        return false;
    }
    if (s_live_tab == 3 &&
        (st.settings.reader_settings_page == kSettingsHome || st.settings.reader_settings_page == kSettingsWriting ||
         st.settings.reader_settings_page == kSettingsTapZones)) {
        return false;
    }
    if (s_live_tab == 0) {
        if (!st.reader.session || !s_toc_count || st.reader.session->TocCount() != s_toc_count ||
            st.reader.session->TocStartsAfterFirstPage() != s_toc_home) {
            return false;
        }
        s_toc_first = -1;
        BindTocRows();
        return true;
    }
    RefreshSwitchRows();
    for (auto& row : s_numeric_rows) {
        if (row.option >= static_cast<int>(BookTextOption::MarginTop) &&
            row.option <= static_cast<int>(BookTextOption::MarginRight)) {
            row.option = static_cast<int>(s_margin_option);
        }
        if (row.option < static_cast<int>(BookTextOption::Count)) {
            row.value = TextValue(static_cast<BookTextOption>(row.option));
        }
        UpdateNumericValue(row);
        UpdateNumericBounds(row);
    }
    for (const auto& binding : s_sheet_bindings) {
        int selected = -1;
        if (binding.event == OnSheetOrientMode) {
            selected = BookReaderPrefsOrient();
        } else if (binding.event == SheetAutoTurnToggle) {
            selected = ReaderAutoTurnEnabled() ? 1 : 0;
        } else if (binding.event == SheetWritingMode) {
            selected = BookReaderPrefsWritingMode();
        } else if (binding.event == SheetTextDirection) {
            selected = BookReaderPrefsTextDirection();
        } else if (binding.event == SheetPageTurn) {
            selected = BookReaderPrefsPageTurn();
        } else if (binding.event == SheetBackground) {
            selected = BookReaderPrefsBackground();
        } else if (binding.event == SheetAntialias) {
            selected = BookReaderPrefsAntialias();
        } else if (binding.event == OnSheetFooterBitToggle) {
            selected = BookReaderPrefsFooterMask();
        } else if (binding.event == OnSheetFooterLayout) {
            selected = BookReaderPrefsFooterLayout();
        } else if (binding.event == OnSheetUnderlineMode) {
            selected = BookReaderPrefsUnderlineMode();
        } else if (binding.event == OnMarginSide) {
            selected = static_cast<int>(s_margin_option);
        } else if (binding.event == TextChoice) {
            selected =
                binding.value >= 10 ? 10 + TextValue(BookTextOption::AaStrength) : TextValue(BookTextOption::Indent);
        } else {
            continue;
        }
        SetLiveSheetButton(binding.button, binding.mask ? (selected & binding.value) != 0 : selected == binding.value);
    }
    if (st.reader.session && s_bookmark_button) {
        const bool ready = SheetNavigationReady();
        const bool active = ready && st.reader.session->CurrentPageBookmarkId() != 0;
        SetLiveSheetButton(s_bookmark_button, active);
        lv_obj_set_style_image_recolor(s_bookmark_icon, active ? UiColorBackground() : UiColorForeground(), 0);
        SetSheetBtnEnabled(s_bookmark_button, ready && st.reader.session->BookmarkStore().Error().empty());
    }
    if (st.reader.session && s_duration_value) {
        auto& session = *st.reader.session;
        const uint32_t seconds = session.ReadingSeconds();
        char value[64];
        std::snprintf(value, sizeof(value), LangString_BOOK_DURATION_HM_FMT, seconds / 3600, seconds % 3600 / 60);
        lv_label_set_text(s_duration_value, value);
        std::snprintf(value, sizeof(value), "%.2f%%", session.ReadingProgressX10() / 10.0);
        lv_label_set_text(s_progress_value, value);
        if (s_progress_slider) {
            lv_slider_set_value(s_progress_slider, session.ReadingProgressX10(), LV_ANIM_OFF);
            const bool ready = SheetNavigationReady();
            SetSheetBtnEnabled(s_chapter_prev, ready && (session.IsPageImagesMode() ? session.HasPrevPage()
                                                                                    : session.CurrentTocIndex() > 0));
            SetSheetBtnEnabled(s_chapter_next, ready && (session.IsPageImagesMode()
                                                             ? session.HasNextPage()
                                                             : session.CurrentTocIndex() + 1 < session.TocCount()));
            if (ready) {
                lv_obj_remove_state(s_progress_slider, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(s_progress_slider, LV_STATE_DISABLED);
            }
        }
    }
    return true;
}

void ResetReadingSheetLiveRefs() {
    s_numeric_refresh_pending = false;
    for (auto& row : s_numeric_rows) {
        CommitNumeric(row);
    }
    s_font_pages = {};
    s_toc_pages = {};
    s_numeric_rows.clear();
    s_switch_rows.clear();
    s_toc_list = nullptr;
    s_toc_previous = s_toc_next = nullptr;
    s_toc_rows.clear();
    s_toc_count = 0;
    s_toc_items = 0;
    s_toc_home = false;
    s_toc_building = false;
    s_toc_first = -1;
    s_sheet_bindings.clear();
    s_live_body = nullptr;
    s_live_tab = -1;
    s_live_font_revision = 0;
    s_bookmark_button = s_bookmark_icon = s_duration_value = s_progress_value = s_progress_slider = s_chapter_prev =
        s_chapter_next = nullptr;
}

int ReadingSheetHeaderHeight() {
    return std::min<int>(std::max<int>(UiSy(56), Book_ItemFont()->line_height + UiSy(20)), LV_VER_RES / 8);
}

static lv_obj_t* CreateReadingResetButton(lv_obj_t* header, lv_event_cb_t event) {
    const int height = ReadingSheetHeaderHeight();
    lv_point_t text_size;
    lv_text_get_size(&text_size, LangString_BOOK_TAP_ZONES_RESET, Book_ItemFont(), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int width = std::max<int>(UiSx(100), text_size.x + UiSx(24));
    auto* reset = SheetButton(header, LangString_BOOK_TAP_ZONES_RESET, width, height - UiSy(8), event);
    lv_obj_set_style_radius(reset, LV_RADIUS_CIRCLE, 0);
    ScreenFitLabel(lv_obj_get_child(reset, 0), width - UiSx(16), height - UiSy(14), Book_ItemFont());
    lv_obj_center(lv_obj_get_child(reset, 0));
    lv_obj_align(reset, LV_ALIGN_RIGHT_MID, -UiSx(12), 0);
    return reset;
}

void BuildReadingSheetHeader(lv_obj_t* header, const char* header_title, lv_event_cb_t back_event, bool back_arrow,
                             lv_event_cb_t reset_event) {
    const int header_h = ReadingSheetHeaderHeight();
    const int title_x = UiSx(56);
    auto* reset = reset_event ? CreateReadingResetButton(header, reset_event) : nullptr;
    lv_obj_update_layout(header);
    const int right_reserve = reset ? lv_obj_get_style_width(reset, LV_PART_MAIN) + UiSx(24) : UiSx(12);
    const int title_width = std::max<int>(1, lv_obj_get_content_width(header) - title_x - right_reserve - 1);
    const auto* title_font = UiFont(UiFontRole::TitleEmphasis);
    auto* title = SheetLabel(header, header_title, title_font);
    ScreenFitLabel(title, title_width, header_h, title_font);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, title_x, 0);
    auto* close = SheetButton(header, "", UiSx(44), header_h - 8, back_event);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_align(close, LV_ALIGN_LEFT_MID, UiSx(8), 0);
    // Draw a fixed-size chevron: its size does not depend on the default icon font.
    static const lv_point_precise_t down_points[] = {{0, 0}, {12, 10}, {24, 0}};
    auto* down = lv_line_create(close);
    static const lv_point_precise_t back_points[] = {{12, 0}, {0, 10}, {12, 20}};
    lv_line_set_points(down, back_arrow ? back_points : down_points, 3);
    lv_obj_set_style_line_width(down, 4, 0);
    lv_obj_set_style_line_color(down, UiColorForeground(), 0);
    lv_obj_set_style_line_rounded(down, true, 0);
    lv_obj_clear_flag(down, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(down);
}
