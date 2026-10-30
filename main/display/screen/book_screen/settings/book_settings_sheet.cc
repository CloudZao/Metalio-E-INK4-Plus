#include "assets/lang_screen.h"
#include "standby_screen/standby_screen.h"
#pragma GCC optimize("O1")

#include "assets/lang_reader.h"

#include "book_screen/settings/book_settings_sheet.h"
#include "book_screen/book_screen_priv.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "epd_flash_guard.h"
#include "lv_adapter_display.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"

#include "reader/txt_chapter.h"
#include "book_screen/settings/book_sheet_icons.h"
#include "book_screen/settings/book_bookmark_art.h"
#include "fontpack_lvgl.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>
#include "book_screen/book_geom.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/settings/book_ttf_actions.h"
#include "book_screen/cover/book_cover_cell.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/cover/book_cover_style.h"
#include <lvgl.h>
#include <dirent.h>

#include "assets/lang_config.h"
#include "screen_text.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "settings.h"
#include "settings_screen/settings_common.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* kFlNvsNs = "frontlight";
constexpr const char* kFlNvsKeyCct = "cct";
constexpr int kFlBrightnessStep = 1;
constexpr const char* kFlTag = "BookFl";

FrontlightCct NormalizeFlCct(int value) {
    if (value < static_cast<int>(FrontlightCct::kWarm) ||
        value > static_cast<int>(FrontlightCct::kOff)) {
        return FrontlightCct::kOff;
    }
    return static_cast<FrontlightCct>(value);
}

void PersistFlCctTask(void* arg) {
    const int cct = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Settings settings(kFlNvsNs, true);
    settings.SetInt(kFlNvsKeyCct, cct);
    ESP_LOGI(kFlTag, "nvs save cct=%d", cct);
    vTaskDelete(nullptr);
}

void RefreshSheetFlBrightnessLabel() {
    auto& st = Book_State();
    if (st.settings.sheet_fl_bright_value == nullptr ||
        !lv_obj_is_valid(st.settings.sheet_fl_bright_value)) {
        return;
    }
    auto& fl = Frontlight::GetInstance();
    const int bri = fl.initialized() ? static_cast<int>(fl.target_brightness()) : 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT, bri);
    lv_label_set_text(st.settings.sheet_fl_bright_value, buf);
}

void ApplySheetFlBrightnessDelta(int delta) {
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        return;
    }
    int next = static_cast<int>(fl.target_brightness()) + delta;
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    if (next == fl.target_brightness() && next == fl.brightness()) {
        return;
    }
    fl.SetBrightness(static_cast<uint8_t>(next), true);
    RefreshSheetFlBrightnessLabel();
}
} // namespace

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
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    Book_DisableScroll(btn);

    lv_obj_t* text_wrap = lv_obj_create(btn);
    lv_obj_remove_style_all(text_wrap);
    lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(text_wrap, 2, 0);
    lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
    lv_obj_set_style_pad_hor(text_wrap, 5, 0);
    lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
    Book_DisableScroll(text_wrap);
    lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(text_wrap);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}
lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSheetIconBtn, kSheetIconBtn);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kSheetBorderW, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_radius(btn, kSheetIconBtn / 2, 0);
    Book_DisableScroll(btn);
    // 始终挂事件，原地 SetSheetBtnEnabled 切换可点态（勿依赖重建）
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    SetSheetBtnEnabled(btn, enabled);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, Book_ListFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void OnSheetTocClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    OpenReadToc();
}
void OnSheetFontPagePrev(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.settings.font_list_page <= 0) {
        return;
    }
    st.settings.font_list_page -= 1;
    RequestSettingsSheetPaint();
}

void OnSheetFontPageNext(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.settings.font_list_page + 1 >= FontListPageCount()) {
        return;
    }
    st.settings.font_list_page += 1;
    RequestSettingsSheetPaint();
}

void OnSheetMarginDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsMarginPreset();
    if (cur <= 0) {
        return;
    }
    BookReaderPrefsSetMarginPreset(cur - 1);
    RequestSettingsSheetPaint();
    ScheduleLayoutApply(false);
}

void OnSheetMarginInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsMarginPreset();
    if (cur + 1 >= kBookReaderMarginPresetCount) {
        return;
    }
    BookReaderPrefsSetMarginPreset(cur + 1);
    RequestSettingsSheetPaint();
    ScheduleLayoutApply(false);
}

bool DeferComicTextSetting() {
    if (!ReaderPageImagesHint()) return false;
    ScreenShowToast(Lang::Strings::BOOK_COMIC_SETTING_SAVED);
    return true;
}

void OnSheetGapDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsSpacingPreset();
    if (cur <= 0) {
        return;
    }
    BookReaderPrefsSetSpacingPreset(cur - 1);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    ScheduleLayoutApply(false);
}

void OnSheetGapInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsSpacingPreset();
    if (cur + 1 >= kBookReaderSpacingPresetCount) {
        return;
    }
    BookReaderPrefsSetSpacingPreset(cur + 1);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    ScheduleLayoutApply(false);
}

void OnSheetFooterBitToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int bit = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const int before = BookReaderPrefsFooterMask();
    const int after = BookReaderPrefsToggleFooterBit(bit);
    RequestSettingsSheetPaint();
    SyncFooterClockTimer();
    if ((before == 0) != (after == 0)) {
        if (Book_State().reader.layout_debounce_timer != nullptr) {
            FlushLayoutDebounce();
        } else {
            ApplyReaderLayoutLive(false);
        }
        return;
    }
    UpdateReadFooter();
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

void OnSheetUnderlineMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        return;
    }
    const int current = BookReaderPrefsUnderlineMode();
    const int next = current == mode ? kBookReaderUnderlineOff : mode;
    if (next == current) return;
    BookReaderPrefsSetUnderlineMode(next);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    RequestRenderReaderPage();
}

void OnSheetFlCct(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto cct = NormalizeFlCct(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        ESP_LOGW(kFlTag, "frontlight not ready");
        return;
    }
    Book_State().settings.reader_eye_mode = false;
    if (fl.cct() == cct) cct = FrontlightCct::kOff;
    fl.SetCct(cct);
    const int value = static_cast<int>(cct);
    if (xTaskCreate(PersistFlCctTask, "book_fl_cct", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(value)), 5, nullptr) != pdPASS) {
        ESP_LOGE(kFlTag, "xTaskCreate(book_fl_cct) failed");
    }
    RequestSettingsSheetPaint();
}

void OnSheetFlBrightDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    ApplySheetFlBrightnessDelta(-kFlBrightnessStep);
}

void OnSheetFlBrightInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    ApplySheetFlBrightnessDelta(kFlBrightnessStep);
}

void BuildReadingBottomSheet();

void RebuildSettingsSheet() {
    if (Book_State().reader.standby_paused) return;
    BuildReadingBottomSheet();
}


namespace {
constexpr int kDurationTab = 4;
struct SheetBinding { lv_obj_t* button;lv_event_cb_t event;int value;bool mask; };
static std::vector<SheetBinding> s_sheet_bindings;
static lv_obj_t* s_auto_turn_value = nullptr;
static lv_obj_t* s_text_direction_row = nullptr;
static int s_auto_turn_seconds = 10;
static bool s_auto_turn_adjusting = false;
static int s_toc_count = 0;
static bool s_toc_building = false;
static lv_obj_t* s_live_body=nullptr;
static lv_obj_t *s_bookmark_button=nullptr,*s_bookmark_icon=nullptr,*s_duration_value=nullptr,*s_progress_value=nullptr,*s_progress_slider=nullptr,*s_chapter_prev=nullptr,*s_chapter_next=nullptr;
static int s_live_tab=-1,s_live_width=0,s_live_height=0;
static lv_coord_t SheetNavHeight() {
    const int height=LV_HOR_RES>LV_VER_RES
        ? std::min<int>(UiSy(74),std::max<int>(Book_ListFont()->line_height,Book_ItemFont()->line_height)+54)
        : UiSy(74);
    return std::min<int>(height,LV_VER_RES/6);
}
static lv_obj_t* SheetLabel(lv_obj_t* parent, const char* text, const lv_font_t* font = nullptr) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font ? font : Book_ItemFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
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
static lv_obj_t* SheetButton(lv_obj_t* parent, const char* text, int w, int h,
                             lv_event_cb_t cb, intptr_t value = 0, bool active = false) {
    auto* btn = SheetBox(parent, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(btn, active ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(value));
    auto* label = SheetLabel(btn, text);
    ScreenFitLabel(label, w - 12, h - 6);
    lv_obj_set_style_text_color(label, active ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(label);
    return btn;
}
static void SheetTabEvent(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    SelectReadingSheetTab(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}
static void SheetClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookLvAsync(AsyncHideReadChrome);
}
static void SheetScrollBegin(lv_event_t* e) {
    auto& settings = Book_State().settings;
    if (lv_event_get_target(e) != settings.reader_sheet_scroll) return;
    settings.reader_scroll_last = lv_obj_get_scroll_y(settings.reader_sheet_scroll);
    settings.reader_sheet_scrolling = true;
    EpdSetUiScrolling(true);
}
static void SheetScrollMove(lv_event_t* e) {
    auto& settings = Book_State().settings;
    auto* list = static_cast<lv_obj_t*>(lv_event_get_target(e));
    static bool adjusting = false;
    if (adjusting || !settings.reader_sheet_scrolling || list != settings.reader_sheet_scroll || !TouchUiFingerIsDown()) return;
    // 仅加速当前输入设备实际拖动的列表，恢复位置和按钮翻页不参与加速。
    auto* indev = lv_indev_active();
    if (!indev || lv_indev_get_scroll_obj(indev) != list) return;
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
    if (target == settings.reader_sheet_scroll && tab >= 0 && tab < 5)
        settings.reader_scroll[tab] = lv_obj_get_scroll_y(target);
    if (target == settings.reader_sheet_scroll && settings.reader_sheet_scrolling) {
        settings.reader_sheet_scrolling = false;
        EpdSetUiScrolling(false);
        const auto& session=Book_State().reader.session;
        if(tab==0 && session && (s_toc_count!=session->TocCount() ||
            s_toc_building!=session->LiveRelayoutPending())) RequestSettingsSheetPaint();
    }
}
static void SheetPreset(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) return;
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (encoded < 10) {
        if (encoded == BookReaderPrefsMarginPreset()) return;
        BookReaderPrefsSetMarginPreset(encoded);
    } else {
        if (encoded - 10 == BookReaderPrefsSpacingPreset()) return;
        BookReaderPrefsSetSpacingPreset(encoded - 10);
    }
    RequestSettingsSheetPaint();
    if (encoded >= 10 && DeferComicTextSetting()) return;
    ScheduleLayoutApply(false);
}
static bool SheetNavigationReady() {
    const auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen() || st.reader.prefs_reopen_pending ||
        st.reader.layout_again || st.reader.opening.load()) return false;
    return !st.reader.deferred_cleanup && !st.reader.standby_paused;
}
static void SheetJumpAsync(void* data) {
    if (!SheetNavigationReady()) return;
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(data));
    const bool ok = encoded >= 10000 ? RequestReaderNavigation(reader::BookSession::NavigationKind::kProgress, encoded - 10000)
                                    : RequestReaderNavigation(reader::BookSession::NavigationKind::kToc, encoded);
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
    auto& st=Book_State();
    if(!SheetNavigationReady() || st.reader.read_chrome!=BookUiState::ReadChrome::kSettings)return;
    const int delta=static_cast<int>(reinterpret_cast<intptr_t>(data));
    const auto kind=st.reader.session->IsPageImagesMode() ? reader::BookSession::NavigationKind::kDelta
                                                        : reader::BookSession::NavigationKind::kChapterDelta;
    if(RequestReaderNavigation(kind, delta)) {
        RequestRenderReaderPage();
        RequestSettingsSheetPaint();
    }
}
static void SheetChapter(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    BookLvAsync(SheetChapterAsync,lv_event_get_user_data(e));
}
static void SheetSeek(lv_event_t* e) {
    const int value = lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    BookLvAsync(SheetJumpAsync, reinterpret_cast<void*>(static_cast<intptr_t>(10000 + value)));
}
static lv_obj_t* SheetSlider(lv_obj_t* parent, int value, int max, int w) {
    auto* slider = lv_slider_create(parent);
    lv_obj_set_size(slider, w, UiSy(16));
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_slider_set_range(slider, 0, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, std::max<int>(0, (UiSx(30) - UiSy(16)) / 2), LV_PART_KNOB);
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
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, UiSx(5), LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(list, SheetScrollMove, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(list, SheetScrollBegin, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(list, SheetScrollEnd, LV_EVENT_SCROLL_END,
        reinterpret_cast<void*>(static_cast<intptr_t>(settings.reader_tab)));
}
static lv_obj_t* SettingsRow(lv_obj_t* parent, const char* title, int count, const char* const* names,
                             lv_event_cb_t event, const int* values, int selected, bool mask = false, int slots = 0) {
    lv_obj_update_layout(parent);
    const int w = lv_obj_get_content_width(parent);
    const int h = lv_obj_get_content_height(parent) / (LV_HOR_RES > LV_VER_RES ? 6 : 12);
    auto* row = SheetBox(parent, w, h);
    auto* label = SheetLabel(row, title);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    const int start = std::min<lv_coord_t>(UiSx(94), w / 3);
    const int gap = UiSx(8);
    ScreenFitLabel(label, start - gap, h - 4);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    const int columns = slots > count ? slots : count;
    const int bw = (w - start - gap * (columns - 1)) / columns;
    for (int i = 0; i < count; ++i) {
        const bool on = mask ? (selected & values[i]) != 0 : selected == values[i];
        auto* btn = SheetButton(row, names[i], bw, std::min<lv_coord_t>(UiSy(26), h - 6), event, values[i], on);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, start + (columns - count + i) * (bw + gap), 0);
        s_sheet_bindings.push_back({btn,event,values[i],mask});
    }
    return row;
}
static void SheetAutoTurnToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    SetReaderAutoTurnEnabled(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    RequestSettingsSheetPaint();
}
static void SheetAutoTurnInterval(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_auto_turn_seconds = BookReaderPrefsAutoTurnSeconds();
        s_auto_turn_adjusting = true;
    }
    if ((code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) && s_auto_turn_adjusting) {
        const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
        s_auto_turn_seconds = std::max(5, std::min(45, s_auto_turn_seconds + delta));
        char text[16]; std::snprintf(text, sizeof(text), "%ds", s_auto_turn_seconds);
        if (s_auto_turn_value) lv_label_set_text(s_auto_turn_value, text);
    }
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_auto_turn_adjusting) {
        s_auto_turn_adjusting = false;
        // Persist once per gesture, rather than writing Flash on every repeat.
        BookReaderPrefsSetAutoTurnSeconds(s_auto_turn_seconds);
        ResetReaderAutoTurnDeadline();
    }
}
static void SheetWritingMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode == BookReaderPrefsWritingMode()) return;
    BookReaderPrefsSetWritingMode(mode);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    ScheduleLayoutApply(false);
}
static void UpdateTextDirectionVisibility() {
    if (!s_text_direction_row) return;
    const bool visible = BookReaderPrefsWritingMode() == kBookReaderWritingVertical;
    lv_obj_set_flag(s_text_direction_row, LV_OBJ_FLAG_HIDDEN, !visible);
}
static bool FitReadingSettingsHeight() {
    auto& settings = Book_State().settings;
    if (settings.reader_tab != 3 || LV_HOR_RES > LV_VER_RES) return false;
    auto* body = settings.reader_sheet_body;
    auto* sheet = settings.settings_sheet;
    lv_obj_update_layout(body);
    int height = lv_obj_get_style_pad_top(body, LV_PART_MAIN) +
        lv_obj_get_style_pad_bottom(body, LV_PART_MAIN);
    for (uint32_t i = 0; i < lv_obj_get_child_count(body); ++i) {
        auto* row = lv_obj_get_child(body, i);
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) height += lv_obj_get_height(row);
    }
    const int sheet_height = lv_obj_get_y(body) + height + SheetNavHeight() + 4;
    if (height == lv_obj_get_height(body) && sheet_height == lv_obj_get_height(sheet)) return false;
    // 保持设置行高度不变，仅随可见行收缩或展开面板。
    lv_obj_set_height(sheet, sheet_height);
    lv_obj_set_height(body, height);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    return true;
}
static void SheetPageTurn(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode == BookReaderPrefsPageTurn()) return;
    BookReaderPrefsSetPageTurn(mode);
    EpdSetReaderEffect(mode == kBookReaderPageTurnWipe ? EpdEffect::Wipe :
                      mode == kBookReaderPageTurnRipple ? EpdEffect::Ripple : EpdEffect::None);
    RequestSettingsSheetPaint();
}
static void SheetTextDirection(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int direction = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (direction == BookReaderPrefsTextDirection()) return;
    BookReaderPrefsSetTextDirection(direction);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    RequestRenderReaderPage();
}
static void SheetBackground(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    BookReaderPrefsSetBackground(BookReaderPrefsBackground() == mode ? 0 : mode);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    RequestRenderReaderPage();
}
static void SheetAntialias(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int enabled = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (enabled == BookReaderPrefsAntialias()) return;
    BookReaderPrefsSetAntialias(enabled);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    RequestRenderReaderPage();
}
static void BuildSheetSettings(lv_obj_t* body) {
    lv_obj_update_layout(body);
    const bool landscape=LV_HOR_RES>LV_VER_RES;
    lv_obj_t* left=body;lv_obj_t* right=body;
    if(landscape) {
        lv_obj_set_layout(body,LV_LAYOUT_NONE);
        const int w=lv_obj_get_content_width(body),h=lv_obj_get_content_height(body);
        left=SheetBox(body,(w-16)/2,h);right=SheetBox(body,(w-16)/2,h);
        lv_obj_set_pos(right,(w+16)/2,0);
        lv_obj_set_flex_flow(left,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_flow(right,LV_FLEX_FLOW_COLUMN);
        auto* divider=SheetBox(body,2,h);lv_obj_set_pos(divider,w/2-1,0);
        lv_obj_set_style_bg_color(divider,lv_color_black(),0);lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);
    }
    const char* directions[] = {Lang::Strings::BOOK_ORIENT_PORTRAIT, Lang::Strings::BOOK_ORIENT_LAND_LEFT, Lang::Strings::BOOK_ORIENT_LAND_RIGHT, Lang::Strings::BOOK_ORIENT_AUTO};
    const int orientation[] = {kBookReaderOrientPortrait, kBookReaderOrientLandLeft,
                               kBookReaderOrientLandRight, kBookReaderOrientAuto};
    SettingsRow(left, Lang::Strings::BOOK_ORIENT_TITLE, 4, directions, OnSheetOrientMode, orientation, BookReaderPrefsOrient());
    const char* writing[] = {Lang::Strings::BOOK_WRITING_HORIZONTAL, Lang::Strings::BOOK_WRITING_VERTICAL};
    const int writing_values[] = {0, 1};
    SettingsRow(left, Lang::Strings::BOOK_WRITING_TITLE, 2, writing, SheetWritingMode, writing_values, BookReaderPrefsWritingMode());
    const char* text_directions[] = {Lang::Strings::BOOK_TEXT_LEFT_TO_RIGHT, Lang::Strings::BOOK_TEXT_RIGHT_TO_LEFT};
    const int text_direction_values[] = {kBookReaderTextLeftToRight, kBookReaderTextRightToLeft};
    s_text_direction_row = SettingsRow(left, Lang::Strings::BOOK_TEXT_DIRECTION, 2, text_directions,
        SheetTextDirection, text_direction_values, BookReaderPrefsTextDirection());
    UpdateTextDirectionVisibility();
    const char* page_turns[] = {Lang::Strings::BOOK_TURN_SLIDE, Lang::Strings::BOOK_TURN_PUSH};
    const int page_turn_values[] = {kBookReaderPageTurnSlide, kBookReaderPageTurnWipe};
    SettingsRow(left, Lang::Strings::BOOK_TURN_TITLE, 2, page_turns, SheetPageTurn, page_turn_values, BookReaderPrefsPageTurn());
    lv_obj_update_layout(left);
    const int auto_w = lv_obj_get_content_width(left);
    const int auto_h = lv_obj_get_content_height(left) / (landscape ? 6 : 12);
    auto* auto_row = SheetBox(left, auto_w, auto_h);
    const int gap = UiSx(8);
    const int title_w = std::min<lv_coord_t>(UiSx(94), auto_w / 3);
    const int toggle_w = (auto_w - title_w - gap * 3) / 4;
    const int button_h = std::min<lv_coord_t>(UiSy(26), auto_h - 6);
    const int step_w = toggle_w / 2;
    const int value_w = 2 * toggle_w - step_w * 2 - gap;
    auto* auto_title = SheetLabel(auto_row, Lang::Strings::BOOK_AUTO_TURN);
    ScreenFitLabel(auto_title, title_w - gap, auto_h - 4);
    lv_obj_set_style_text_align(auto_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(auto_title, LV_ALIGN_LEFT_MID, 0, 0);
    int auto_x = title_w;
    for (int enabled = 0; enabled <= 1; ++enabled) {
        auto* button = SheetButton(auto_row, enabled ? Lang::Strings::COMMON_ON : Lang::Strings::COMMON_OFF, toggle_w, button_h,
                                   SheetAutoTurnToggle, enabled, ReaderAutoTurnEnabled() == (enabled != 0));
        lv_obj_align(button, LV_ALIGN_LEFT_MID, auto_x, 0);
        s_sheet_bindings.push_back({button, SheetAutoTurnToggle, enabled, false});
        auto_x += toggle_w + gap;
    }
    auto* minus = SheetButton(auto_row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC, step_w, button_h, SheetAutoTurnInterval, -1);
    lv_obj_align(minus, LV_ALIGN_LEFT_MID, auto_x, 0);auto_x += step_w + gap;
    s_auto_turn_seconds = BookReaderPrefsAutoTurnSeconds();
    char auto_text[16];std::snprintf(auto_text, sizeof(auto_text), "%ds", s_auto_turn_seconds);
    s_auto_turn_value = SheetLabel(auto_row, auto_text);
    lv_obj_set_width(s_auto_turn_value, value_w);
    lv_obj_set_style_text_align(s_auto_turn_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_auto_turn_value, LV_ALIGN_LEFT_MID, auto_x, 0);auto_x += value_w + gap;
    auto* plus = SheetButton(auto_row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_INC, step_w, button_h, SheetAutoTurnInterval, 1);
    lv_obj_align(plus, LV_ALIGN_LEFT_MID, auto_x, 0);
    for (auto* button : {minus, plus}) {
        lv_obj_remove_event_cb(button, SheetAutoTurnInterval);
        const intptr_t delta = button == minus ? -1 : 1;
        lv_obj_add_event_cb(button, SheetAutoTurnInterval, LV_EVENT_ALL, reinterpret_cast<void*>(delta));
    }
    const char* presets[] = {Lang::Strings::BOOK_PRESET_SMALL, Lang::Strings::BOOK_SPACING_STANDARD, Lang::Strings::BOOK_PRESET_LARGE, Lang::Strings::BOOK_PRESET_XLARGE}; const int values[] = {0,1,2,3};
    SettingsRow(left, Lang::Strings::BOOK_MARGIN_TITLE, 4, presets, SheetPreset, values, BookReaderPrefsMarginPreset());
    const int spacing[] = {10,11,12,13};
    SettingsRow(right, Lang::Strings::BOOK_SPACING_TITLE, 4, presets, SheetPreset, spacing, 10 + BookReaderPrefsSpacingPreset());
    const char* backgrounds[] = {Lang::Strings::BOOK_BACKGROUND_GRID, Lang::Strings::BOOK_BACKGROUND_DOTS}; const int bg_values[] = {1,2};
    SettingsRow(right, Lang::Strings::BOOK_BACKGROUND_TITLE, 2, backgrounds, SheetBackground, bg_values, BookReaderPrefsBackground());
    const char* foot[] = {Lang::Strings::BOOK_FOOTER_TITLE, Lang::Strings::BOOK_FOOTER_TIME, Lang::Strings::BOOK_FOOTER_PROGRESS, Lang::Strings::BOOK_FOOTER_BATTERY};
    const int bits[] = {kBookReaderFooterTitle,kBookReaderFooterTime,kBookReaderFooterProgress,kBookReaderFooterBattery};
    SettingsRow(right, Lang::Strings::BOOK_CHAPTER_PROGRESS, 4, foot, OnSheetFooterBitToggle, bits, BookReaderPrefsFooterMask(), true);
    const char* underline[] = {Lang::Strings::BOOK_UNDERLINE_SOLID, Lang::Strings::BOOK_UNDERLINE_DASHED}; const int ul[] = {1,2};
    SettingsRow(right, Lang::Strings::BOOK_UNDERLINE, 2, underline, OnSheetUnderlineMode, ul, BookReaderPrefsUnderlineMode());
    const char* antialias[] = {Lang::Strings::COMMON_OFF, Lang::Strings::COMMON_ON}; const int aa_values[] = {0, 1};
    SettingsRow(right, Lang::Strings::BOOK_ANTIALIAS, 2, antialias, SheetAntialias, aa_values, BookReaderPrefsAntialias());
    const int one[] = {0};
    const char* custom[] = {Lang::Strings::BOOK_CUSTOM};
    SettingsRow(right, Lang::Strings::BOOK_TAP_ZONES_TITLE, 1, custom, OnSheetTapZonesClicked, one, -1, false, 2);
}
// Only viewport rows exist; a one-pixel extent preserves the full scroll range.
static lv_obj_t* s_toc_list = nullptr;
static std::vector<lv_obj_t*> s_toc_rows;
static int s_toc_height = 1, s_toc_first = -1;
static bool s_toc_binding = false;
static void BindTocRows();
static void TocScroll(lv_event_t*) { BindTocRows(); }

// Fixed frame and navigation; only the inner list scrolls.
static void SheetListPageState(lv_event_t* e) {
    auto* list=static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* holder=lv_obj_get_parent(lv_obj_get_parent(list));
    if(lv_obj_get_child_count(holder)<3)return;
    const int current=lv_obj_get_scroll_y(list);
    lv_area_t bounds;lv_obj_get_coords(list,&bounds);
    bool more=false;
    for(uint32_t i=0;i<lv_obj_get_child_count(list);++i) {
        lv_area_t row;lv_obj_get_coords(lv_obj_get_child(list,i),&row);
        if(row.y2>bounds.y2)more=true;
    }
    if(list==s_toc_list)more=current+lv_obj_get_height(list)<s_toc_count*s_toc_height;
    auto* previous=lv_obj_get_child(holder,1);auto* next=lv_obj_get_child(holder,2);
    if(current>0)lv_obj_remove_state(previous,LV_STATE_DISABLED);else lv_obj_add_state(previous,LV_STATE_DISABLED);
    if(more)lv_obj_remove_state(next,LV_STATE_DISABLED);else lv_obj_add_state(next,LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(lv_obj_get_child(previous,0),current>0?LV_OPA_COVER:LV_OPA_40,0);
    lv_obj_set_style_text_opa(lv_obj_get_child(next,0),more?LV_OPA_COVER:LV_OPA_40,0);
}
static void SheetListPage(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto* button=static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* holder=lv_obj_get_parent(button);
    auto* list=lv_obj_get_child(lv_obj_get_child(holder,0),0);
    lv_obj_update_layout(list);
    const int current=lv_obj_get_scroll_y(list);
    const int viewport=lv_obj_get_height(list);
    if(list==s_toc_list) {
        const int rows=std::max(1,viewport/s_toc_height);
        const int direction=reinterpret_cast<intptr_t>(lv_event_get_user_data(e))>0?1:-1;
        if(direction>0 && current+viewport>=s_toc_count*s_toc_height)return;
        const int first=std::clamp(current/s_toc_height+direction*rows,0,std::max(0,s_toc_count-1));
        lv_obj_scroll_to_y(list,first*s_toc_height,LV_ANIM_OFF);
        Book_State().settings.reader_scroll[0]=lv_obj_get_scroll_y(list);
        return;
    }
    lv_area_t bounds;lv_obj_get_coords(list,&bounds);
    int target=current;
    const bool next=reinterpret_cast<intptr_t>(lv_event_get_user_data(e))>0;
    if(next) {
        target=current+viewport;
        for(uint32_t i=0;i<lv_obj_get_child_count(list);++i) {
            lv_area_t row;lv_obj_get_coords(lv_obj_get_child(list,i),&row);
            const int top=row.y1-bounds.y1+current;
            const int bottom=row.y2-bounds.y1+current+1;
            if(bottom>current+viewport) {target=top>current?top:current+viewport;break;}
        }
    } else {
        // Start at a row boundary, retaining the partially visible top row.
        int anchor=current;
        for(uint32_t i=0;i<lv_obj_get_child_count(list);++i) {
            lv_area_t row;lv_obj_get_coords(lv_obj_get_child(list,i),&row);
            const int top=row.y1-bounds.y1+current;
            if(row.y2>=bounds.y1) {anchor=top;break;}
        }
        target=std::max(0,anchor-viewport);
        for(uint32_t i=0;i<lv_obj_get_child_count(list);++i) {
            lv_area_t row;lv_obj_get_coords(lv_obj_get_child(list,i),&row);
            const int top=row.y1-bounds.y1+current;
            if(top>=target) {target=top;break;}
        }
        if(target>=current)target=std::max(0,current-viewport);
    }
    if(next) {
        int last_end=0;
        for(uint32_t i=0;i<lv_obj_get_child_count(list);++i) {
            lv_area_t row;lv_obj_get_coords(lv_obj_get_child(list,i),&row);
            last_end=std::max<int>(last_end,row.y2-bounds.y1+current+1);
        }
        if(last_end<=current+viewport)return;
    }
    // Permit blank space on the final page so its first row stays aligned.
    lv_obj_scroll_to_y(list,std::max(0,target),LV_ANIM_OFF);
    auto& settings=Book_State().settings;
    settings.reader_scroll[settings.reader_tab]=lv_obj_get_scroll_y(list);
}
static lv_obj_t* SheetPagedList(lv_obj_t* parent,int width,int height) {
    height=std::max(1,height-6); // Gap between the fixed frame and bottom card rule.
    const int nav=std::max<int>(UiSy(44),Book_ItemFont()->line_height+16);
    auto* holder=SheetBox(parent,width,height);
    // One fixed outline encloses both the scrolling list and navigation band.
    lv_obj_set_style_border_width(holder,2,0);
    lv_obj_set_style_border_color(holder,lv_color_black(),0);
    lv_obj_set_style_border_post(holder,true,0);
    lv_obj_set_style_radius(holder,UiSx(6),0);
    lv_obj_set_style_clip_corner(holder,true,0);
    auto* frame=SheetBox(holder,width-4,std::max(1,height-nav-4));
    auto* list=SheetBox(frame,width-4,std::max(1,height-nav-4));
    ConfigureSheetScroll(list);
    auto* previous=SheetButton(holder,"<<",UiSx(88),nav-1,SheetListPage,-1);
    auto* next=SheetButton(holder,">>",UiSx(88),nav-1,SheetListPage,1);
    lv_obj_set_style_border_width(previous,0,0);lv_obj_set_style_border_width(next,0,0);
    lv_obj_set_style_radius(previous,0,0);lv_obj_set_style_radius(next,0,0);
    lv_obj_set_pos(previous,0,height-nav-3);lv_obj_set_pos(next,width-4-UiSx(88),height-nav-3);
    auto* separator=SheetBox(holder,width-4,1);
    lv_obj_set_pos(separator,0,height-nav-4);
    lv_obj_set_style_bg_color(separator,lv_color_black(),0);
    lv_obj_set_style_bg_opa(separator,LV_OPA_COVER,0);
    lv_obj_add_event_cb(list,SheetListPageState,LV_EVENT_SCROLL,nullptr);
    // Extra bottom padding allows the last partially visible item to become page start.
    lv_obj_set_style_pad_bottom(list,std::max(0,height-nav-4),0);
    return list;
}
static void BuildSheetFonts(lv_obj_t* body) {
    auto& st = Book_State();
    lv_obj_update_layout(body);
    auto* list = SheetPagedList(body,LV_HOR_RES-UiSx(32),lv_obj_get_content_height(body));
    if (st.settings.font_entries.size() <= kFontListPageSize) {
        lv_obj_set_style_pad_bottom(list, 0, 0);
        Book_DisableScroll(list);
    }
    if (st.settings.font_scan_loading || st.settings.font_scan_failed) {
        lv_obj_set_style_pad_bottom(list, 0, 0);
        Book_ShowMessage(list, st.settings.font_scan_loading ?
            Lang::Strings::BOOK_FONT_LOADING : Lang::Strings::BOOK_FONT_REOPEN, Book_ItemFont());
        return;
    }
    if (st.settings.font_entries.empty()) {
        lv_obj_set_style_pad_bottom(list, 0, 0);
        Book_ShowMessage(list, Lang::Strings::BOOK_FONT_IMPORT_EMPTY, Book_ItemFont());
        return;
    }
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* current_row=nullptr;
    lv_obj_update_layout(list);
    const int page_height = lv_obj_get_height(list);
    for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
        const auto& font = st.settings.font_entries[i];
        const int slot = i % kFontListPageSize;
        const int row_height = (slot + 1) * page_height / kFontListPageSize -
            slot * page_height / kFontListPageSize;
        auto* row = SheetBox(list, LV_HOR_RES - UiSx(32) - 2, row_height);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, OnSheetFontSelect, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(row, OnSheetFontLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
        char name[SD_FILE_NAME_CAPACITY]; FormatFontListName(font.file, name, sizeof(name));
        const bool current = font.file == BookReaderPrefsFontFile();
        const bool selected = current && !st.settings.font_multi;
        if (current) current_row = row;
        const auto* row_font=selected?Book_ListFont():Book_ItemFont();
        const int check_offset = st.settings.font_multi ? UiSx(32) : 0;
        if (st.settings.font_multi) {
            auto* check = SheetBox(row, UiSx(22), UiSx(22));
            lv_obj_set_style_border_width(check, 1, 0);
            lv_obj_set_style_border_color(check, lv_color_black(), 0);
            lv_obj_align(check, LV_ALIGN_LEFT_MID, UiSx(12), 0);
            if (FontItemSelected(i)) lv_obj_center(SheetLabel(check, "√"));
        }
        auto* title = SheetLabel(row, name, row_font);
        lv_obj_set_width(title, LV_HOR_RES - UiSx(130) - check_offset); lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, UiSx(12) + check_offset, 0);
        char px[20]; std::snprintf(px, sizeof(px), "%dPX", font.size_px);
        auto* size = SheetLabel(row, px,row_font);lv_obj_align(size,LV_ALIGN_RIGHT_MID,-UiSx(12),0);
        if(selected) {
            // Match the current chapter's one-pixel overstrike with the same font and clipping.
            auto* bold_title=SheetLabel(row,name,row_font);
            lv_obj_set_width(bold_title,LV_HOR_RES-UiSx(130));
            lv_label_set_long_mode(bold_title,LV_LABEL_LONG_DOT);
            lv_obj_align(bold_title,LV_ALIGN_LEFT_MID,UiSx(12)+1,0);
            auto* bold_size=SheetLabel(row,px,row_font);
            lv_obj_align(bold_size,LV_ALIGN_RIGHT_MID,-UiSx(12)+1,0);
        }
    }
    lv_obj_update_layout(list);
    auto* last_row = lv_obj_get_child(list, -1);
    const int maximum = last_row ? std::max<int>(0, lv_obj_get_y(last_row)) : 0;
    const int requested = st.settings.reader_scroll[1] < 0
        ? (current_row ? lv_obj_get_y(current_row) : 0) : st.settings.reader_scroll[1];
    // 页尾留白仅用于将末行对齐到页首，恢复位置不能越过末行而显示纯空白。
    st.settings.reader_scroll[1] = std::clamp(requested, 0, maximum);

}
static void BuildSheetToc(lv_obj_t* body) {
    auto& st = Book_State(); auto& session = *st.reader.session;
    // Chapter metadata is already loaded. Submit the complete card once;
    // flushing a transient hint starts panel power-up during row allocation.
    lv_obj_update_layout(body);
    const bool landscape=LV_HOR_RES>LV_VER_RES;
    const int body_w=lv_obj_get_content_width(body),body_h=lv_obj_get_content_height(body);
    if(landscape)lv_obj_set_layout(body,LV_LAYOUT_NONE);
    const int head_w=landscape?(body_w-16)/2:body_w;
    auto* head = SheetBox(body, head_w, landscape?body_h:UiSy(184));
    const int ch=landscape?body_h*3/5:UiSy(172);
    const int cw=landscape?ch*140/172:UiSx(140); // Wider cover in both orientations; height stays unchanged.
    const auto& info = session.Info();
    bool fill = false;
    auto* cover = CreateCoverSlot(head, info, cw, ch, &st.settings.reader_sheet_cover, &fill);
    lv_obj_align(cover, LV_ALIGN_LEFT_MID, 0, 0);
    if (fill) {
        EnqueueListCoverFill(info, CoverFrameInner(cw), CoverFrameInner(ch), cover, &st.settings.reader_sheet_cover);
        ScheduleListCoverFill();
    }
    auto* title = SheetLabel(head, session.Title().c_str(), Book_ListFont());
    const int info_x=cw+UiSx(20);
    const int cover_top=landscape?(body_h-ch)/2:0;
    lv_obj_set_width(title,head_w-info_x);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title,info_x,landscape?cover_top+ch*20/172:UiSy(20));
    if(landscape)lv_obj_set_height(title,Book_ListFont()->line_height*2);
    std::string author_name = session.Author();
    const auto first = author_name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) author_name = Lang::Strings::COMMON_UNKNOWN;
    else author_name = author_name.substr(first, author_name.find_last_not_of(" \t\r\n") - first + 1);
    if (author_name == "未知" || author_name == "未命名" || author_name == "unknown" || author_name == "Unknown" || author_name == "null" || author_name == Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC || author_name == "　") author_name = Lang::Strings::COMMON_UNKNOWN;
    auto* author = SheetLabel(head, author_name.c_str());
    lv_obj_set_size(author,head_w-info_x,Book_ItemFont()->line_height);
    lv_label_set_long_mode(author,LV_LABEL_LONG_DOT);
    lv_obj_set_pos(author,info_x,landscape?cover_top+ch-Book_ItemFont()->line_height-6:UiSy(142));
    lv_obj_update_layout(body);
    const int width = landscape?(body_w-16)/2:body_w;
    auto* list = SheetPagedList(body,width,landscape?body_h:std::max<int>(1,body_h-UiSy(184)));
    if(landscape) {
        lv_obj_set_pos(lv_obj_get_parent(lv_obj_get_parent(list)),(body_w+16)/2,0);
        auto* divider=SheetBox(body,2,body_h);lv_obj_set_pos(divider,body_w/2-1,0);
        lv_obj_set_style_bg_color(divider,lv_color_black(),0);lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);
    }
    s_toc_list=list;
    s_toc_count=session.TocCount();
    s_toc_building=session.LiveRelayoutPending();
    s_toc_height=std::max<int>(UiSy(44),std::max(Book_ListFont()->line_height,Book_ItemFont()->line_height)+UiSy(12));
    s_toc_first=-1;
    lv_obj_set_layout(list,LV_LAYOUT_NONE);
    lv_obj_set_style_pad_bottom(list,0,0);
    lv_obj_update_layout(list);
    if (!s_toc_count) {
        Book_ShowMessage(list, session.LiveRelayoutPending() ?
            Lang::Strings::BOOK_TOC_BUILDING : Lang::Strings::BOOK_TOC_EMPTY, Book_ItemFont());
        return;
    }
    const int viewport=lv_obj_get_height(list);
    auto* extent=SheetBox(list,1,1);
    lv_obj_set_pos(extent,0,(s_toc_count-1)*s_toc_height+viewport-1);
    const int pool=std::min(s_toc_count,(viewport+s_toc_height-1)/s_toc_height+2);
    for(int slot=0;slot<pool;++slot) {
        auto* row=SheetButton(list,"",width,s_toc_height,SheetTocJump,0);
        for(int label=0;label<3;++label)SheetLabel(row,"");
        s_toc_rows.push_back(row);
    }
    if(st.settings.reader_scroll[0]<0)
        st.settings.reader_scroll[0]=std::max(0,session.CurrentTocIndex())*s_toc_height;
    lv_obj_add_event_cb(list,TocScroll,LV_EVENT_SCROLL,nullptr);
    BindTocRows();

}

static void BindTocRows() {
    if(!s_toc_list || s_toc_binding || !s_toc_count)return;
    const int first=std::clamp<int>(lv_obj_get_scroll_y(s_toc_list)/s_toc_height,0,std::max(0,s_toc_count-int(s_toc_rows.size())));
    if(first==s_toc_first)return;
    s_toc_binding=true;s_toc_first=first;
    auto& session=*Book_State().reader.session;
    const int width=lv_obj_get_width(s_toc_list);
    for(size_t slot=0;slot<s_toc_rows.size();++slot) {
        const int i=first+slot;
        const auto* entry = session.TocAt(i); if (!entry) continue;
        std::string ordinal, name;
        const bool split = reader::SplitChapterHeading(entry->title, ordinal, name);
        const auto* font = i == session.CurrentTocIndex() ? Book_ListFont() : Book_ItemFont();
        const int ordinal_w = split ? std::min(width / 2, std::max<int>(UiSx(90), Book_MeasureTextWidth(font, ordinal.c_str()) + UiSx(12))) : 0;
        const int text_w = width - ordinal_w - UiSx(16);
        auto* row=s_toc_rows[slot];
        lv_obj_remove_flag(row,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(row,i*s_toc_height);
        lv_obj_remove_event_cb(row,SheetTocJump);
        lv_obj_add_event_cb(row,SheetTocJump,LV_EVENT_CLICKED,reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        auto* text = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_font(text, font, 0);
        lv_obj_set_size(text, text_w, font->line_height);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_label_set_text(text, split ? name.c_str() : entry->title.c_str());
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(text, LV_ALIGN_LEFT_MID, ordinal_w + UiSx(8), 0);
        for(int label=1;label<4;++label)lv_obj_add_flag(lv_obj_get_child(row,label),LV_OBJ_FLAG_HIDDEN);
        if (i == session.CurrentTocIndex()) {
            auto* bold = lv_obj_get_child(row,1);
            lv_obj_remove_flag(bold,LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(bold,font,0);
            lv_label_set_text(bold,split?name.c_str():entry->title.c_str());
            lv_obj_set_size(bold, text_w, font->line_height);
            lv_label_set_long_mode(bold, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(bold, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(bold, LV_ALIGN_LEFT_MID, ordinal_w + UiSx(8) + 1, 0);
        }
        if (split) {
            auto* number=lv_obj_get_child(row,2);
            lv_obj_remove_flag(number,LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(number,font,0);
            lv_label_set_text(number,ordinal.c_str());
            lv_obj_set_size(number, ordinal_w, font->line_height);
            lv_label_set_long_mode(number, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(number, LV_ALIGN_LEFT_MID, 0, 0);
            if (i == session.CurrentTocIndex()) {
                auto* bold_number=lv_obj_get_child(row,3);
                lv_obj_remove_flag(bold_number,LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_font(bold_number,font,0);
                lv_label_set_text(bold_number,ordinal.c_str());
                lv_obj_set_size(bold_number, ordinal_w, font->line_height);
                lv_label_set_long_mode(bold_number, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(bold_number, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(bold_number, LV_ALIGN_LEFT_MID, 1, 0);
            }
        }
    }
    s_toc_binding=false;
}

enum BookmarkAction { kBookmarkAdd,kBookmarkNext,kBookmarkCancel,kBookmarkAll,kBookmarkRemove,kBookmarkClick,kBookmarkLong,kBookmarkPrevious };
struct BookmarkRequest { BookmarkAction action;uint32_t id;std::string path; };
static lv_obj_t* s_bookmark_message = nullptr;
static void BookmarkMessage(const std::string& text) {
    if (StandbyScreen::IsActive()) return;
    CloseBookmarkMessage();
    auto* box=lv_msgbox_create(nullptr);
    s_bookmark_message = box;
    lv_obj_add_event_cb(box, [](lv_event_t* event) {
        if (lv_event_get_target(event) == s_bookmark_message) s_bookmark_message = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    auto* title=lv_msgbox_add_title(box,Lang::Strings::BOOK_BOOKMARKS);
    auto* label=lv_msgbox_add_text(box,text.c_str());
    lv_obj_set_style_text_font(title,Book_ListFont(),0);
    lv_obj_set_style_text_font(label,Book_ItemFont(),0);
    lv_msgbox_add_close_button(box);
}
static void BookmarkRequestAsync(void* data) {
    std::unique_ptr<BookmarkRequest> request(static_cast<BookmarkRequest*>(data));
    auto& st=Book_State();auto* session=st.reader.session.get();
    if(StandbyScreen::IsActive() || !session || !session->IsOpen() || session->Info().path!=request->path ||
       st.reader.read_chrome!=BookUiState::ReadChrome::kSettings)return;
    try {
    auto& selected=st.settings.bookmark_selected;
    if(request->action==kBookmarkCancel)ResetBookmarkSelection();
    else if(request->action==kBookmarkNext || request->action==kBookmarkPrevious) {
        const int groups=std::max<int>(1,(session->BookmarkStore().Entries().size()+5)/6);
        const int delta=request->action==kBookmarkNext?1:-1;
        st.settings.bookmark_group=std::max(0,std::min(groups-1,st.settings.bookmark_group+delta));
        st.settings.reader_scroll[2]=0;
    } else if(request->action==kBookmarkAll) {
        selected.clear();for(const auto& entry:session->BookmarkStore().Entries())selected.push_back(entry.id);
    } else if(request->action==kBookmarkLong || (request->action==kBookmarkClick && st.settings.bookmark_multi)) {
        const auto& entries=session->BookmarkStore().Entries();
        if(std::none_of(entries.begin(),entries.end(),[&](const reader::BookBookmark& entry){return entry.id==request->id;}))return;
        const auto found=std::find(selected.begin(),selected.end(),request->id);
        if(request->action==kBookmarkLong) {
            st.settings.bookmark_multi=true;
            if(found==selected.end())selected.push_back(request->id);
        } else if(found==selected.end())selected.push_back(request->id);
        else selected.erase(found);
    } else {
        if(!SheetNavigationReady()) {
            BookmarkMessage(Lang::Strings::BOOK_LAYOUT_WAIT);return;
        }
        std::string error;
        if(request->action==kBookmarkAdd) {
            const uint32_t current=session->CurrentPageBookmarkId();
            const bool ok=current?session->RemoveBookmarks({current},error):session->AddCurrentPageBookmark(error);
            if(!ok) { BookmarkMessage(error);return; }
        } else if(request->action==kBookmarkRemove) {
            if(selected.empty())return;
            if(!session->RemoveBookmarks(selected,error)) { BookmarkMessage(error);return; }
            ResetBookmarkSelection();
        } else if(request->action==kBookmarkClick) {
            if(!RequestReaderNavigation(reader::BookSession::NavigationKind::kBookmark, request->id)) { BookmarkMessage(Lang::Strings::BOOK_BOOKMARK_LOCATE_FAILED);return; }
            HideReadChrome();RequestRenderReaderPage();return;
        }
    }
    RequestSettingsSheetPaint();
    } catch(const std::bad_alloc&) { BookmarkMessage(Lang::Strings::COMMON_MEMORY_RETRY); }
}
static void QueueBookmarkAction(BookmarkAction action,uint32_t id=0) {
    auto& st=Book_State();if(!st.reader.session)return;
    try {
    auto* request=new(std::nothrow) BookmarkRequest{action,id,st.reader.session->Info().path};
    if(request && !BookLvAsync(BookmarkRequestAsync,request))delete request;
    } catch(const std::bad_alloc&) { BookmarkMessage(Lang::Strings::COMMON_MEMORY_RETRY); }
}
static void BookmarkActionEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    QueueBookmarkAction(static_cast<BookmarkAction>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}
static void BookmarkContentEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    QueueBookmarkAction(lv_event_get_code(event)==LV_EVENT_LONG_PRESSED?kBookmarkLong:kBookmarkClick,
                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}
static void BindBookmarkContent(lv_obj_t* object,uint32_t id) {
    lv_obj_add_flag(object,LV_OBJ_FLAG_CLICKABLE);HapticAttachClick(object);
    // SHORT_CLICKED never follows LONG_PRESSED, so release cannot undo selection.
    lv_obj_add_event_cb(object,BookmarkContentEvent,LV_EVENT_SHORT_CLICKED,reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    lv_obj_add_event_cb(object,BookmarkContentEvent,LV_EVENT_LONG_PRESSED,reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
}
static void BuildSheetBookmarks(lv_obj_t* body) {
    auto& st=Book_State();auto& session=*st.reader.session;
    const auto& entries=session.BookmarkStore().Entries();
    lv_obj_update_layout(body);
    const int width=lv_obj_get_content_width(body);
    const bool landscape=LV_HOR_RES>LV_VER_RES;
    const int height=lv_obj_get_content_height(body);
    auto* page=SheetBox(body,width,height);
    const auto x=[width,landscape](int value) { return value*(landscape?(width-16)/2:width)/701; };
    const auto y=[height](int value) { return value*height/1245; };
    const lv_font_t* font=Book_ItemFont();
    const int footer_h=landscape?font->line_height+28:0;
    const int content_h=height-footer_h;
    const int column_w=(width-16)/2;
    const auto add_art_pixels=[&](const lv_image_dsc_t* src,int ax,int ay,int aw,int ah) {
        auto* slot=SheetBox(page,aw,ah);
        lv_obj_set_pos(slot,ax,ay);
        auto* image=lv_image_create(slot);
        lv_image_set_src(image,src);
        const int scale=std::min(aw*256/src->header.w,ah*256/src->header.h);
        lv_image_set_scale(image,std::max(1,scale));
        lv_obj_center(image);
        lv_obj_clear_flag(image,LV_OBJ_FLAG_CLICKABLE);
    };
    const auto add_art=[&](const lv_image_dsc_t* src,int ax,int ay,int aw,int ah) {
        add_art_pixels(src,x(ax),y(ay),x(aw),y(ah));
    };
    if(landscape) {
        const int art_h=content_h*3/5,art_w=art_h*3/8;
        add_art_pixels(BookmarkTitleImage(),column_w-art_w-14,(content_h-art_h)/2,art_w,art_h);
        auto* divider=SheetBox(page,2,content_h);lv_obj_set_pos(divider,width/2-1,0);
        lv_obj_set_style_bg_color(divider,lv_color_black(),0);lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);
    } else add_art(BookmarkTitleImage(),570,236,90,240);
    bool fill=false;const int ch=landscape?content_h*7/10:y(332),cw=landscape?ch*232/332:x(232);
    // Bound a -10 degree rotation around the cover centre, with rounding slack.
    const int frame_w=static_cast<int>(std::ceil(cw*0.984807753+ch*0.173648178))+8;
    const int frame_h=static_cast<int>(std::ceil(ch*0.984807753+cw*0.173648178))+8;
    auto* cover_frame=SheetBox(page,frame_w,frame_h);
    const int cover_x=landscape?(width+16)/2+12:x(40)+(x(260)-frame_w)/2;
    const int cover_y=landscape?(content_h-frame_h)/2:y(740)+(y(326)-frame_h)/2;
    lv_obj_set_pos(cover_frame,cover_x,cover_y);
    auto* cover=CreateCoverSlot(cover_frame,session.Info(),cw,ch,&st.settings.reader_sheet_cover,&fill);
    lv_obj_center(cover);
    lv_obj_set_style_transform_pivot_x(cover,cw/2,0);
    lv_obj_set_style_transform_pivot_y(cover,ch/2,0);
    lv_obj_set_style_transform_rotation(cover,-100,0);
    if(fill) {
        EnqueueListCoverFill(session.Info(),CoverFrameInner(cw),CoverFrameInner(ch),cover,&st.settings.reader_sheet_cover);
        ScheduleListCoverFill();
    } else if(st.settings.reader_sheet_cover.empty()) {
        auto* name=lv_obj_get_child(cover,0);
        lv_label_set_text(name,session.Title().c_str());
        lv_obj_set_width(name,cw-UiSx(12));lv_obj_set_height(name,Book_ItemFont()->line_height*4);
        lv_obj_set_style_text_font(name,Book_ItemFont(),0);
        lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
        lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);lv_obj_center(name);
    }
    if(landscape) add_art_pixels(BookmarkClipImage(),cover_x+frame_w*3/5,cover_y-8,36,36);
    else add_art(BookmarkClipImage(),200,704,66,66);
    struct Sample { int x,width,dot_x; };
    const Sample samples[] = {
        {112,294,462}, {112,294,462}, {112,294,462},
        {398,270,354}, {398,270,354}, {398,270,354},
    };
    // Equal-height excerpt slots and a fixed row stride prevent long text from
    // crowding the next bookmark. Keep three lines at most, with ellipsis.
    const int line_height=font->line_height+2;
    const int text_h=3*line_height-2;
    const int row_step=landscape?std::max(text_h+24,(content_h-16)/3):std::max(y(180),text_h+y(24)+6);
    if(!landscape)lv_obj_set_height(page,std::max(height,y(70)+5*row_step+text_h+y(80)));
    if(entries.empty()) {
        auto* empty=SheetLabel(page,session.BookmarkStore().Error().empty()?Lang::Strings::BOOK_BOOKMARKS_EMPTY:session.BookmarkStore().Error().c_str());
        lv_obj_set_width(empty,landscape?column_w-24:width*3/5);
        lv_obj_set_style_text_align(empty,LV_TEXT_ALIGN_CENTER,0);
        if(landscape) {
            lv_obj_update_layout(empty);
            lv_obj_set_pos(empty,12,std::max<int>(0,(content_h-lv_obj_get_height(empty))/2));
        } else {
            lv_obj_set_pos(empty,x(110),y(120));
        }
    }
    const int groups=std::max<int>(1,(entries.size()+5)/6);
    st.settings.bookmark_group=std::min(st.settings.bookmark_group,groups-1);
    for(int i=0;i<6;++i) {
        const size_t index=st.settings.bookmark_group*6+i;if(index>=entries.size())break;
        const auto& entry=entries[index];const auto& sample=samples[i];
        const int row_top=landscape?8+(i%3)*row_step:y(70)+i*row_step;
        const int column_x=landscape && i>=3?(width+16)/2:0;
        // All three excerpts in each group share the same text and dot columns.
        // Extend away from the dot/decorations so the existing separation stays intact.
        const int extra_width=i<3?x(40):std::max(0,std::min(x(40),width-column_x-x(sample.x)-x(sample.width)-8));
        const int text_x=column_x+x(sample.x)-(i<3?extra_width:0);
        const int text_w=x(sample.width)+extra_width;
        // Match the sheet's bold text with a one-pixel overstrike.
        for(int offset=0;offset<2;++offset) {
            auto* label=SheetLabel(page,entry.excerpt.c_str(),font);
            BindBookmarkContent(label,entry.id);
            lv_obj_set_size(label,text_w,text_h);
            lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
            lv_obj_set_style_text_line_space(label,2,0);
            lv_obj_set_pos(label,text_x+offset,row_top);
        }
        auto* line=SheetBox(page,text_w,2);
        lv_obj_set_pos(line,text_x,row_top+text_h+4);
        lv_obj_set_style_bg_color(line,lv_color_black(),0);
        lv_obj_set_style_bg_opa(line,LV_OPA_COVER,0);
        const int dot_size=st.settings.bookmark_multi?std::max(22,x(24)):std::max(10,x(18));
        auto* dot=SheetBox(page,dot_size,dot_size);
        lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(dot,st.settings.bookmark_multi?lv_color_white():lv_color_black(),0);
        lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
        lv_obj_set_pos(dot,column_x+x(sample.dot_x)-dot_size/2,row_top+text_h/2-dot_size/2);
        BindBookmarkContent(dot,entry.id);
        if(st.settings.bookmark_multi) {
            lv_obj_set_style_border_width(dot,2,0);lv_obj_set_style_border_color(dot,lv_color_black(),0);
            if(std::find(st.settings.bookmark_selected.begin(),st.settings.bookmark_selected.end(),entry.id)!=st.settings.bookmark_selected.end()) {
                static const lv_point_precise_t points[]={{0,5},{4,9},{11,0}};
                auto* check=lv_line_create(dot);lv_line_set_points(check,points,3);
                lv_obj_set_style_line_width(check,2,0);lv_obj_set_style_line_color(check,lv_color_black(),0);lv_obj_center(check);
                lv_obj_clear_flag(check,LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
    if(st.settings.bookmark_group>0) {
        auto* previous=SheetButton(page,"<<",x(64),std::max<int>(UiSy(30),font->line_height+8),BookmarkActionEvent,kBookmarkPrevious);
        lv_obj_set_style_border_width(previous,0,0);
        lv_obj_align(previous,LV_ALIGN_BOTTOM_LEFT,x(16),landscape?-8:-y(18));
    }
    if(st.settings.bookmark_group+1<groups) {
        auto* more=SheetButton(page,">>",x(64),std::max<int>(UiSy(30),font->line_height+8),BookmarkActionEvent,kBookmarkNext);
        lv_obj_set_style_border_width(more,0,0);
        lv_obj_align(more,LV_ALIGN_BOTTOM_RIGHT,-x(16),landscape?-8:-y(18));
    }
    if(groups>1) {
        char text[24];std::snprintf(text,sizeof(text),"%d / %d",st.settings.bookmark_group+1,groups);
        auto* count=SheetLabel(page,text,font);lv_obj_align(count,LV_ALIGN_BOTTOM_MID,0,landscape?-12:-y(20));
    }
}

static void BuildSheetDuration(lv_obj_t* body) {
    auto& session = *Book_State().reader.session;
    const int width = LV_HOR_RES - UiSx(32);
    auto* row = SheetBox(body, width, UiSy(62));
    const uint32_t seconds = session.ReadingSeconds();
    char duration[64]; std::snprintf(duration, sizeof(duration), Lang::Strings::BOOK_DURATION_HM_FMT, seconds / 3600, seconds % 3600 / 60);
    char progress[64]; const int pct = session.ReadingProgressX10();
    std::snprintf(progress, sizeof(progress), "%.2f%%",
        session.ReadingProgressX10() / 10.0);
    const char* titles[] = {Lang::Strings::BOOK_READ_DURATION, Lang::Strings::BOOK_READING_PROGRESS};
    const char* values[] = {duration, progress};
    for (int i = 0; i < 2; ++i) {
        auto* column = SheetBox(row, width / 2, UiSy(62));
        lv_obj_set_pos(column, i * width / 2, 0);
        auto* title = SheetLabel(column, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UiSy(5));
        auto* value = SheetLabel(column, values[i], Book_ListFont());
        if(i==0)s_duration_value=value;else s_progress_value=value;
        lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -UiSy(5));
    }
    auto* line = SheetBox(row, 1, UiSy(44));lv_obj_set_style_bg_color(line,lv_color_black(),0);
    lv_obj_set_style_bg_opa(line,LV_OPA_COVER,0);lv_obj_center(line);
    auto* controls = SheetBox(body, width, UiSy(44));
    const bool comic = session.IsPageImagesMode();
    const char* previous_title = comic ? Lang::Strings::BOOK_PAGE_PREV : Lang::Strings::BOOK_CHAPTER_PREV;
    const char* next_title = comic ? Lang::Strings::BOOK_PAGE_NEXT : Lang::Strings::BOOK_CHAPTER_NEXT;
    lv_point_t previous_size, next_size;
    lv_text_get_size(&previous_size, previous_title, Book_ItemFont(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&next_size, next_title, Book_ItemFont(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int chapter_w = std::min<int>(width / 3, std::max(previous_size.x, next_size.x) + 16);
    auto* prev = SheetButton(controls, previous_title, chapter_w, UiSy(30), SheetChapter, -1);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 0);
    auto* next = SheetButton(controls, next_title, chapter_w, UiSy(30), SheetChapter, 1);
    s_chapter_prev=prev;s_chapter_next=next;
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_border_width(prev, 0, 0);
    lv_obj_set_style_border_width(next, 0, 0);
    auto* slider = SheetSlider(controls, pct, 1000, width - 2 * chapter_w - UiSx(24));lv_obj_center(slider);
    s_progress_slider=slider;
    lv_obj_add_event_cb(slider, SheetSeek, LV_EVENT_RELEASED, nullptr);
    const bool ready = SheetNavigationReady();
    SetSheetBtnEnabled(prev, ready && (comic ? session.HasPrevPage() : session.CurrentTocIndex() > 0));
    SetSheetBtnEnabled(next, ready && (comic ? session.HasNextPage() : session.CurrentTocIndex() + 1 < session.TocCount()));
    if (!ready) lv_obj_add_state(slider, LV_STATE_DISABLED);
}
} // namespace

bool BookmarkMessageVisible() { return s_bookmark_message != nullptr; }
bool CloseBookmarkMessage() {
    if (!s_bookmark_message) return false;
    auto* box = s_bookmark_message;
    s_bookmark_message = nullptr;
    lv_msgbox_close(box);
    return true;
}


void ResetReadingSheetLiveRefs() {
    s_text_direction_row = nullptr;
    s_toc_list=nullptr;s_toc_rows.clear();s_toc_count=0;s_toc_building=false;s_toc_first=-1;
    s_sheet_bindings.clear();s_live_body=nullptr;s_live_tab=-1;
    if (s_auto_turn_adjusting) BookReaderPrefsSetAutoTurnSeconds(s_auto_turn_seconds);
    s_auto_turn_adjusting=false;s_auto_turn_value=nullptr;
    s_bookmark_button=s_bookmark_icon=s_duration_value=s_progress_value=s_progress_slider=s_chapter_prev=s_chapter_next=nullptr;
}
static void SetLiveSheetButton(lv_obj_t* button,bool active) {
    lv_obj_set_style_bg_color(button,active?lv_color_black():lv_color_white(),0);
    auto* label=lv_obj_get_child(button,0);
    if(label)lv_obj_set_style_text_color(label,active?lv_color_white():lv_color_black(),0);
}
bool RefreshReadingSheetInPlace() {
    auto& st=Book_State();
    if(!s_live_body || s_live_body!=st.settings.reader_sheet_body || !lv_obj_is_valid(s_live_body) ||
       s_live_tab!=st.settings.reader_tab ||
       s_live_width!=LV_HOR_RES || s_live_height!=LV_VER_RES ||
       (s_live_tab!=0 && s_live_tab!=3 && s_live_tab!=kDurationTab))return false;
    if(s_live_tab==0) {
        if(!st.reader.session || !s_toc_count || st.reader.session->TocCount()!=s_toc_count)return false;
        s_toc_first=-1;
        BindTocRows();
        return true;
    }
    UpdateTextDirectionVisibility();
    if (FitReadingSettingsHeight()) ApplyReadingPanelRules(st.settings.settings_sheet, SheetNavHeight() + 2);
    for(const auto& binding:s_sheet_bindings) {
        int selected=-1;
        if(binding.event==SheetPreset)selected=binding.value<10?BookReaderPrefsMarginPreset():10+BookReaderPrefsSpacingPreset();
        else if(binding.event==OnSheetOrientMode)selected=BookReaderPrefsOrient();
        else if(binding.event==SheetAutoTurnToggle)selected=ReaderAutoTurnEnabled()?1:0;
        else if(binding.event==SheetWritingMode)selected=BookReaderPrefsWritingMode();
        else if(binding.event==SheetTextDirection)selected=BookReaderPrefsTextDirection();
        else if(binding.event==SheetPageTurn)selected=BookReaderPrefsPageTurn();
        else if(binding.event==SheetBackground)selected=BookReaderPrefsBackground();
        else if(binding.event==SheetAntialias)selected=BookReaderPrefsAntialias();
        else if(binding.event==OnSheetFooterBitToggle)selected=BookReaderPrefsFooterMask();
        else if(binding.event==OnSheetUnderlineMode)selected=BookReaderPrefsUnderlineMode();
        else continue;
        SetLiveSheetButton(binding.button,binding.mask?(selected&binding.value)!=0:selected==binding.value);
    }
    if(st.reader.session && s_bookmark_button) {
        const bool ready=SheetNavigationReady();
        const bool active=ready && st.reader.session->CurrentPageBookmarkId()!=0;
        SetLiveSheetButton(s_bookmark_button,active);
        lv_obj_set_style_image_recolor(s_bookmark_icon,active?lv_color_white():lv_color_black(),0);
        SetSheetBtnEnabled(s_bookmark_button,ready && st.reader.session->BookmarkStore().Error().empty());
    }
    if(st.reader.session && s_duration_value) {
        auto& session=*st.reader.session;
        const uint32_t seconds=session.ReadingSeconds();
        char value[64];std::snprintf(value,sizeof(value),Lang::Strings::BOOK_DURATION_HM_FMT,seconds/3600,seconds%3600/60);
        lv_label_set_text(s_duration_value,value);
        std::snprintf(value,sizeof(value),"%.2f%%",session.ReadingProgressX10()/10.0);
        lv_label_set_text(s_progress_value,value);
        if(s_progress_slider) {
            lv_slider_set_value(s_progress_slider,session.ReadingProgressX10(),LV_ANIM_OFF);
            const bool ready=SheetNavigationReady();
            SetSheetBtnEnabled(s_chapter_prev,ready && (session.IsPageImagesMode() ? session.HasPrevPage() : session.CurrentTocIndex()>0));
            SetSheetBtnEnabled(s_chapter_next,ready && (session.IsPageImagesMode() ? session.HasNextPage() : session.CurrentTocIndex()+1<session.TocCount()));
            if(ready)lv_obj_remove_state(s_progress_slider,LV_STATE_DISABLED);
            else lv_obj_add_state(s_progress_slider,LV_STATE_DISABLED);
        }
    }
    return true;
}
void ResetBookmarkSelection() {
    Book_State().settings.bookmark_multi=false;
    Book_State().settings.bookmark_selected.clear();
}
bool CancelBookmarkMultiSelection() {
    if(!Book_State().settings.bookmark_multi)return false;
    ResetBookmarkSelection();RequestSettingsSheetPaint();return true;
}
void ApplyReadingPanelRules(lv_obj_t* panel, int bottom_gap) {
    if(!panel || !lv_obj_is_valid(panel))return;
    constexpr int thickness=2, inset=8;
    static int rule_tags[2];
    lv_obj_update_layout(panel);
    auto* status=Book_State().reader.status_bar;
    bool adjoining=false;
    if(status && lv_obj_is_valid(status)) {
        // ScreenCreateStatusBar creates its bottom separator as the first child.
        auto* status_rule=lv_obj_get_child(status,0);
        if(status_rule) {
            lv_obj_set_size(status_rule,lv_obj_get_width(panel)-2*inset,thickness);
            lv_obj_align(status_rule,LV_ALIGN_BOTTOM_MID,0,lv_obj_get_style_pad_bottom(status,LV_PART_MAIN));
        }
        lv_area_t panel_area,status_area;
        lv_obj_get_coords(panel,&panel_area);lv_obj_get_coords(status,&status_area);
        adjoining=panel_area.y1<=status_area.y2+1;
    }
    for(int edge=0;edge<2;++edge) {
        lv_obj_t* rule=nullptr;
        for(uint32_t i=0;i<lv_obj_get_child_count(panel);++i) {
            auto* child=lv_obj_get_child(panel,i);
            if(lv_obj_get_user_data(child)==&rule_tags[edge]) {rule=child;break;}
        }
        if(!rule) {
            rule=lv_obj_create(panel);lv_obj_remove_style_all(rule);
            lv_obj_set_user_data(rule,&rule_tags[edge]);
            lv_obj_add_flag(rule,LV_OBJ_FLAG_FLOATING);
            Book_DisableScroll(rule);lv_obj_clear_flag(rule,LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(rule,lv_color_black(),0);
            lv_obj_set_style_bg_opa(rule,LV_OPA_COVER,0);
        }
        lv_obj_set_size(rule,lv_obj_get_width(panel)-2*inset,thickness);
        // Explicit positions compensate padding and fix x at the same physical inset.
        lv_obj_set_pos(rule,inset-lv_obj_get_style_pad_left(panel,LV_PART_MAIN),
            (edge?lv_obj_get_height(panel)-bottom_gap-thickness:0)-lv_obj_get_style_pad_top(panel,LV_PART_MAIN));
        if(!edge && adjoining)lv_obj_add_flag(rule,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(rule,LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(rule);
    }
}
void ResizeReadingBottomSheet() {
    auto& st = Book_State();auto* sheet = st.settings.settings_sheet;
    if (!sheet || !lv_obj_is_valid(sheet)) return;
    if (st.settings.reader_tab == 3 && LV_HOR_RES <= LV_VER_RES &&
        s_live_body && s_live_body == st.settings.reader_sheet_body && lv_obj_is_valid(s_live_body) &&
        s_live_width == LV_HOR_RES && s_live_height == LV_VER_RES) {
        UpdateTextDirectionVisibility();
        FitReadingSettingsHeight();
        ApplyReadingPanelRules(sheet, SheetNavHeight() + 2);
        return;
    }
    const bool compact = st.settings.reader_tab == kDurationTab;
    const bool full = st.settings.reader_tab == 0 || st.settings.reader_tab == 2 ||
        (st.settings.reader_tab == 3 && LV_HOR_RES > LV_VER_RES);
    int desired = compact ? UiSy(190) :
        full ? LV_VER_RES : st.settings.reader_tab == 3 ? UiSy(558) : UiSy(430);
    if (st.settings.reader_tab == 1) {
        const int header = std::min<int>(UiSy(56), LV_VER_RES / 8);
        const int pager = std::max<int>(UiSy(44), Book_ItemFont()->line_height + 16);
        desired = header + SheetNavHeight() + 4 + 2 * UiSy(4) + 6 + 4 + pager +
            kFontListPageSize * kFontListRowH;
    }
    const int limit = compact ? LV_VER_RES * 3 / 5 : full || st.settings.reader_tab == 1
        ? LV_VER_RES : LV_VER_RES * 4 / 5;
    const int status_h = st.reader.status_h > 0 ? st.reader.status_h : 40;
    const int available = std::max<int>(1, LV_VER_RES - status_h);
    lv_obj_set_size(sheet, LV_HOR_RES, std::min<int>(desired, std::min(limit, available)));
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
}
void SelectReadingSheetTab(int tab) {
    if (tab < 0 || tab > 3) return;
    const auto& settings = Book_State().settings;
    if (tab != 1 && tab == settings.reader_tab && s_live_tab == tab && s_live_body &&
        s_live_body == settings.reader_sheet_body && lv_obj_is_valid(s_live_body) &&
        s_live_width == LV_HOR_RES && s_live_height == LV_VER_RES) return;
    if(tab!=2)ResetBookmarkSelection();
    TapZonesPanelClose();TtfPanelClose();
    Book_State().settings.reader_tab = tab;
    if (tab == 1) StartReadFontScan();
    else CancelReadFontScan();
    if (tab == 0 || tab == 1) Book_State().settings.reader_scroll[tab] = -1;
    RebuildSettingsSheet();
}
void BuildReadingBottomSheet() {
    auto& st = Book_State();auto* sheet = st.settings.settings_sheet;
    if (!sheet || !lv_obj_is_valid(sheet) || !st.reader.session) return;
    EpdFlashGuard display_guard;
    if(st.settings.bookmark_ui_path!=st.reader.session->Info().path) {
        ResetBookmarkSelection();st.settings.bookmark_group=0;st.settings.reader_scroll[2]=0;
        st.settings.bookmark_ui_path=st.reader.session->Info().path;
    }
    CancelListCoverFill();
    ClearSettingsSheetWidgetRefs();lv_obj_clean(sheet);
    s_sheet_bindings.reserve(40);
    st.settings.reader_sheet_cover.Reset();
    lv_obj_remove_style_all(sheet);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(sheet, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_border_color(sheet, lv_color_black(), 0);
    Book_DisableScroll(sheet);lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ResizeReadingBottomSheet();
    const bool duration = st.settings.reader_tab == kDurationTab;
    const bool landscape_bookmarks=st.settings.reader_tab==2 && LV_HOR_RES>LV_VER_RES;
    const int header_h = duration ? 0 : landscape_bookmarks
        ? std::min<int>(UiSy(56),Book_ListFont()->line_height+12)
        : std::min<lv_coord_t>(UiSy(56), LV_VER_RES / 8);
    const int nav_h = SheetNavHeight()+4; // Keep icons at their existing height; add 4px above them.
    if (!duration) {
        auto* header = SheetBox(sheet, LV_HOR_RES, header_h);
        const char* titles[] = {Lang::Strings::BOOK_TOC, Lang::Strings::BOOK_FONT_SELECT, Lang::Strings::BOOK_BOOKMARKS, Lang::Strings::BOOK_READING_SETTINGS};
        char font_selection[32];
        const char* header_title = titles[st.settings.reader_tab];
        if (st.settings.reader_tab == 1 && st.settings.font_multi) {
            std::snprintf(font_selection, sizeof(font_selection), Lang::Strings::BOOK_SELECTED_FMT, FontSelectedCount());
            header_title = font_selection;
        }
        auto* title = SheetLabel(header, header_title, Book_ListFont());lv_obj_center(title);
        if ((st.settings.reader_tab >= 0 && st.settings.reader_tab <= 3)) {
            auto* bold_title = SheetLabel(header, header_title, Book_ListFont());
            lv_obj_align(bold_title, LV_ALIGN_CENTER, 1, 0);
        }
        auto* close = SheetButton(header, "", UiSx(44), header_h - 8, SheetClose);
        lv_obj_set_style_border_width(close, 0, 0);lv_obj_align(close, LV_ALIGN_LEFT_MID, UiSx(8), 0);
        // Draw a fixed-size chevron: its size does not depend on the default icon font.
        static const lv_point_precise_t down_points[] = {
            {0, 0}, {12, 10}, {24, 0}
        };
        auto* down = lv_line_create(close);
        lv_line_set_points(down, down_points, 3);
        lv_obj_set_style_line_width(down, 4, 0);
        lv_obj_set_style_line_color(down, lv_color_black(), 0);
        lv_obj_set_style_line_rounded(down, true, 0);
        lv_obj_clear_flag(down, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(down);
        if (st.settings.reader_tab == 1 && !st.settings.font_multi) {
            auto* action = SheetButton(header,Lang::Strings::BOOK_IMPORT_TTF,
                UiSx(112),std::min<int>(UiSy(34),header_h-8),OnFontImportClick);
            lv_obj_set_ext_click_area(action, UiSx(6));
            lv_obj_align(action, LV_ALIGN_RIGHT_MID, -UiSx(12), 0);
            SetSheetBtnEnabled(action, !st.settings.font_scan_loading);
        }
        if(st.settings.reader_tab==3) {
            const bool ready=SheetNavigationReady();
            const bool active=ready && st.reader.session->CurrentPageBookmarkId()!=0;
            const int size=std::min<int>(UiSy(36),header_h-8);
            auto* action=SheetButton(header,"",size,size,BookmarkActionEvent,kBookmarkAdd,active);
            lv_obj_set_style_radius(action,LV_RADIUS_CIRCLE,0);
            lv_obj_align(action,LV_ALIGN_RIGHT_MID,-UiSx(12),0);
            auto* icon=lv_image_create(action);lv_image_set_src(icon,SheetBookmarkIcon());
            s_bookmark_button=action;s_bookmark_icon=icon;
            lv_obj_clear_flag(icon,LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_image_recolor(icon,active?lv_color_white():lv_color_black(),0);
            lv_obj_set_style_image_recolor_opa(icon,LV_OPA_COVER,0);
            lv_obj_center(icon);SetSheetBtnEnabled(action,ready && st.reader.session->BookmarkStore().Error().empty());
        }
        if(st.settings.reader_tab==2 && st.settings.bookmark_multi) {
            char count[32];std::snprintf(count,sizeof(count),Lang::Strings::BOOK_SELECTED_ITEMS_FMT,static_cast<unsigned>(st.settings.bookmark_selected.size()));
            auto* label=SheetLabel(header,count,Book_ListFont());
            lv_obj_set_style_bg_color(label,lv_color_white(),0);lv_obj_set_style_bg_opa(label,LV_OPA_COVER,0);lv_obj_center(label);
        }
    }
    lv_obj_update_layout(sheet);
    auto* body = SheetBox(sheet, LV_HOR_RES, lv_obj_get_height(sheet) - header_h - nav_h);
    st.settings.reader_sheet_body = body;lv_obj_set_pos(body, 0, header_h);
    lv_obj_set_style_pad_hor(body, UiSx(16), 0);
    lv_obj_set_style_pad_ver(body, st.settings.reader_tab == 0 || landscape_bookmarks ? 0 : UiSy(4), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(body, 0, 0);
    if (st.settings.reader_tab == kDurationTab) ConfigureSheetScroll(body);
    if (duration) lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_update_layout(body);
    if (st.settings.reader_tab == 0) BuildSheetToc(body);
    else if (st.settings.reader_tab == 1) BuildSheetFonts(body);
    else if (st.settings.reader_tab == kDurationTab) BuildSheetDuration(body);
    else if (st.settings.reader_tab == 2) BuildSheetBookmarks(body);
    else BuildSheetSettings(body);
    auto* nav = SheetBox(sheet, LV_HOR_RES, nav_h);lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_border_color(nav, lv_color_black(), 0);
    const char* tabs[] = {Lang::Strings::BOOK_TOC, Lang::Strings::BOOK_FONT_TITLE, Lang::Strings::BOOK_BOOKMARKS, Lang::Strings::HOME_APP_SETTINGS};
    const lv_image_dsc_t* icons[] = {SheetFontIcon(), SheetTocIcon(), SheetBookmarkNavIcon(), SheetSettingsIcon()};
    const int bottom_gap = LV_HOR_RES>LV_VER_RES?8:UiSy(10); // Additional space inside the display's configured 8px content inset.
    if(st.settings.reader_tab==2 && st.settings.bookmark_multi) {
        const char* actions[]={Lang::Strings::COMMON_CANCEL,Lang::Strings::COMMON_SELECT_ALL,Lang::Strings::COMMON_REMOVE};
        const BookmarkAction codes[]={kBookmarkCancel,kBookmarkAll,kBookmarkRemove};
        for(int i=0;i<3;++i) {
            auto* button=SheetButton(nav,actions[i],LV_HOR_RES/3-UiSx(16),std::min<int>(UiSy(34),nav_h-bottom_gap-6),BookmarkActionEvent,codes[i]);
            lv_obj_set_pos(button,LV_HOR_RES*i/3+UiSx(8),UiSy(16));
            if(i==2)SetSheetBtnEnabled(button,!st.settings.bookmark_selected.empty());
        }
    } else if (st.settings.reader_tab == 1 &&
               (st.settings.font_multi || !st.ttf.ttf_selected.empty())) {
        const bool ttf_multi = !st.ttf.ttf_selected.empty();
        const char* actions[] = {Lang::Strings::COMMON_CANCEL, Lang::Strings::COMMON_SELECT_ALL, Lang::Strings::COMMON_REMOVE};
        const lv_event_cb_t callbacks[] = {
            ttf_multi ? OnTtfMultiCancel : OnFontMultiCancel,
            ttf_multi ? OnTtfMultiSelectAll : OnFontMultiSelectAll,
            ttf_multi ? OnTtfMultiRemove : OnFontMultiRemove};
        for (int i = 0; i < 3; ++i) {
            auto* button = SheetButton(nav, actions[i], LV_HOR_RES / 3 - UiSx(16),
                std::min<int>(UiSy(34), nav_h - bottom_gap - 6), callbacks[i]);
            lv_obj_set_pos(button, LV_HOR_RES * i / 3 + UiSx(8), UiSy(16));
            if (i == 2) SetSheetBtnEnabled(button,
                (ttf_multi ? TtfSelectedCount() : FontSelectedCount()) > 0 && (ttf_multi || !ReaderWorkersBusy()));
        }
    } else for (int i = 0; i < 4; ++i) {
        auto* btn = SheetBox(nav, LV_HOR_RES / 4, nav_h - bottom_gap - 2);lv_obj_set_pos(btn, i * LV_HOR_RES / 4, 1);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, SheetTabEvent, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        auto* icon = lv_image_create(btn);
        lv_image_set_src(icon, icons[i]);
        if(i==2)lv_obj_set_style_image_recolor(icon,lv_color_black(),0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, LV_HOR_RES>LV_VER_RES?8:UiSy(5)+4);
        auto* label = SheetLabel(btn, tabs[i], st.settings.reader_tab == i ? Book_ListFont() : Book_ItemFont());lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, LV_HOR_RES>LV_VER_RES?-6:-UiSy(8));
        if (st.settings.reader_tab == i) {
            // The bundled UI font has no bold face: a one-pixel overstrike thickens its strokes.
            auto* bold = SheetLabel(btn, tabs[i], Book_ListFont());
            lv_obj_align(bold, LV_ALIGN_BOTTOM_MID, 1, LV_HOR_RES>LV_VER_RES?-6:-UiSy(8));
            auto* mark = SheetBox(btn, UiSx(44), UiSy(4));
            lv_obj_set_style_bg_color(mark, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
            lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
    }
    lv_obj_update_layout(body);
    if (st.settings.reader_sheet_scroll) {
        lv_obj_scroll_to_y(st.settings.reader_sheet_scroll, st.settings.reader_scroll[st.settings.reader_tab], LV_ANIM_OFF);
        if(st.settings.reader_tab==0 || st.settings.reader_tab==1)
            lv_obj_send_event(st.settings.reader_sheet_scroll,LV_EVENT_SCROLL,nullptr);
    }
    s_live_body=body;s_live_tab=st.settings.reader_tab;
    s_live_width=LV_HOR_RES;s_live_height=LV_VER_RES;
    FitReadingSettingsHeight();
    ApplyReadingPanelRules(sheet,nav_h-2);
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) TtfEnsurePanel();
    if (st.tap.tap_zone_ui != BookUiState::TapZoneUi::kClosed) TapZonesEnsurePanel();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) SetReadStatusVisible(true);
}


