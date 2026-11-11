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
#include <initializer_list>
#include <strings.h>
#include <new>
#include <vector>
#include "book_screen/book_geom.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/settings/book_tap_zones.h"
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
    {
        Settings settings(kFlNvsNs, true);
        settings.SetInt(kFlNvsKeyCct, cct);
    }
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
    const int next = mode;
    if (next == current) return;
    BookReaderPrefsSetUnderlineMode(next);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    CancelReaderPageWork();
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
constexpr int kReadingCardSideInset = 4; // 主卡片左右内收的物理像素
constexpr int kReadingCardBorderWidth = 2; // 顶部、两侧及导航分隔线宽度
struct SheetBinding { lv_obj_t* button;lv_event_cb_t event;int value;bool mask; };
static std::vector<SheetBinding> s_sheet_bindings;
static int s_toc_count = 0;
constexpr int kSheetProgressBase = 10000; // 进度跳转编码起点，与目录索引区分
static int s_toc_items = 0; // 包含各页首页入口的显示项总数
static bool s_toc_home = false; // 目录页是否为首页入口预留一行
static bool s_toc_building = false;
static lv_obj_t* s_live_body=nullptr;
static lv_obj_t *s_bookmark_button=nullptr,*s_bookmark_icon=nullptr,*s_duration_value=nullptr,*s_progress_value=nullptr,*s_progress_slider=nullptr,*s_chapter_prev=nullptr,*s_chapter_next=nullptr;
static int s_live_tab=-1,s_live_width=0,s_live_height=0;
static int s_live_settings_page = -1;
static lv_coord_t SheetNavHeight() {
    const int height=LV_HOR_RES>LV_VER_RES
        ? std::min<int>(UiSy(74),std::max<int>(Book_ListFont()->line_height,Book_ItemFont()->line_height)+54)
        : UiSy(74);
    return std::min<int>(height,LV_VER_RES/6);
}
static bool ReadingCardStyle() {
    const int tab = Book_State().settings.reader_tab;
    return tab == 1 || tab == 3 || tab == kDurationTab;
}
// 阅读菜单沿用现有界面字，不新增字号或字重。
static const lv_font_t* SheetUiFont(int size, bool bold = false, const char* text = nullptr) {
    (void)size;
    (void)bold;
    (void)text;
    return Book_ItemFont();
}
static void DisabledAdjustmentDraw(lv_event_t* event) {
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (!lv_obj_has_state(button, LV_STATE_DISABLED)) return;
    lv_area_t area; lv_obj_get_coords(button, &area);
    lv_area_increase(&area, -1, -1);
    const int radius = UiSx(8), dash = UiSx(4), pitch = UiSx(8);
    auto* layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
    line.color = lv_color_black(); line.width = 2;
    for (int x = area.x1 + radius; x < area.x2 - radius; x += pitch) {
        line.p1 = {x, area.y1}; line.p2 = {std::min<int>(x + dash, area.x2 - radius), area.y1};
        lv_draw_line(layer, &line);
        line.p1.y = line.p2.y = area.y2; lv_draw_line(layer, &line);
    }
    for (int y = area.y1 + radius; y < area.y2 - radius; y += pitch) {
        line.p1 = {area.x1, y}; line.p2 = {area.x1, std::min<int>(y + dash, area.y2 - radius)};
        lv_draw_line(layer, &line);
        line.p1.x = line.p2.x = area.x2; lv_draw_line(layer, &line);
    }
    lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_black(); arc.width = 2; arc.radius = radius;
    const lv_point_t centers[] = {{area.x2-radius, area.y2-radius}, {area.x1+radius, area.y2-radius},
        {area.x1+radius, area.y1+radius}, {area.x2-radius, area.y1+radius}};
    for (int corner = 0; corner < 4; ++corner) for (int angle = 0; angle < 90; angle += 30) {
        arc.center = centers[corner]; arc.start_angle = corner * 90 + angle;
        arc.end_angle = arc.start_angle + 15; lv_draw_arc(layer, &arc);
    }
}
static lv_obj_t* SheetLabel(lv_obj_t* parent, const char* text, const lv_font_t* font = nullptr) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font ? font : ReadingCardStyle() ? SheetUiFont(20, false, text) : Book_ItemFont(), 0);
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
static void SyncButtonEmphasis(lv_obj_t* button, bool active) {
    auto* label = lv_obj_get_child(button, 0);
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    const char* text = lv_label_get_text(label);
    auto* copy = static_cast<lv_obj_t*>(lv_obj_get_user_data(label));
    if (!active || text == nullptr || text[0] == '\0') {
        if (copy && lv_obj_is_valid(copy)) lv_obj_add_flag(copy, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!copy || !lv_obj_is_valid(copy)) {
        copy = lv_label_create(button);
        lv_obj_set_user_data(label, copy);
        lv_obj_add_flag(copy, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
        lv_obj_clear_flag(copy, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_clear_flag(copy, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(copy, text);
    lv_obj_set_style_text_font(copy, lv_obj_get_style_text_font(label, LV_PART_MAIN), 0);
    lv_obj_set_style_text_color(copy, lv_obj_get_style_text_color(label, LV_PART_MAIN), 0);
    lv_obj_set_style_text_align(copy, lv_obj_get_style_text_align(label, LV_PART_MAIN), 0);
    lv_obj_set_size(copy, std::max<int>(1, lv_obj_get_width(label)), std::max<int>(1, lv_obj_get_height(label)));
    lv_label_set_long_mode(copy, lv_label_get_long_mode(label));
    lv_obj_update_layout(button);
    lv_obj_set_pos(copy, lv_obj_get_x(label) + 1, lv_obj_get_y(label));
}
static lv_obj_t* SheetButton(lv_obj_t* parent, const char* text, int w, int h,
                             lv_event_cb_t cb, intptr_t value = 0, bool active = false,
                             lv_coord_t radius = LV_RADIUS_CIRCLE, bool align_left = false) {
    auto* btn = SheetBox(parent, w, h);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(btn, active ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(value));
    else lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    auto* label = SheetLabel(btn, text);
    const int inset = align_left ? UiSx(12) : 6;
    ScreenFitLabel(label, std::max<int>(1, w - static_cast<int>(inset) * 2), h - 6, ReadingCardStyle() ? Book_ItemFont() : nullptr);
    lv_obj_set_style_text_color(label, active ? lv_color_white() : lv_color_black(), 0);
    if (align_left) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, inset, 0);
    } else {
        lv_obj_center(label);
    }
    SyncButtonEmphasis(btn, active);
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
            s_toc_building!=session->LiveRelayoutPending() ||
            s_toc_home!=session->TocStartsAfterFirstPage())) RequestSettingsSheetPaint();
    }
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
    const bool progress = encoded >= kSheetProgressBase;
    const auto kind = progress ? reader::BookSession::NavigationKind::kProgress
                               : reader::BookSession::NavigationKind::kToc;
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
    BookLvAsync(SheetJumpAsync, reinterpret_cast<void*>(static_cast<intptr_t>(kSheetProgressBase + value)));
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
static int SettingsColumn(lv_obj_t* parent, std::initializer_list<const char*> captions) {
    const auto* font = Book_ItemFont();
    int text = static_cast<int>(UiSx(72));
    if (font) {
        for (const char* caption : captions) {
            if (caption && *caption)
                text = std::max<int>(text, static_cast<int>(Book_MeasureTextWidth(font, caption)) + static_cast<int>(UiSx(8)));
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
                             lv_event_cb_t event, const int* values, int selected, bool mask = false,
                             int slots = 0, int column = 0, int row_height = 0) {
    lv_obj_update_layout(parent);
    const int w = lv_obj_get_content_width(parent);
    const int h = row_height > 0 ? row_height : UiSy(60);
    auto* row = SheetBox(parent, w, h);
    auto* label = SheetLabel(row, title);
    const int start = column > 0 ? column : SettingsColumn(parent, {title});
    PlaceSettingsCaption(label, start);
    const int gap = UiSx(8);
    const int columns = slots > count ? slots : count;
    const int bw = std::max<int>(1, (static_cast<int>(w) - start - static_cast<int>(gap) * (columns - 1)) / columns);
    for (int i = 0; i < count; ++i) {
        const bool on = mask ? (selected & values[i]) != 0 : selected == values[i];
        auto* btn = SheetButton(row, names[i], bw, UiSy(40), event, values[i], on, UiSx(7));
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, start + (columns - count + i) * (bw + gap), 0);
        s_sheet_bindings.push_back({btn,event,values[i],mask});
    }
    return row;
}
enum SettingsPage { kSettingsHome, kSettingsRotation, kSettingsFooter, kSettingsBackground,
    kSettingsWriting, kSettingsAntialias, kSettingsSpacing, kSettingsUnderline, kSettingsTurning };
constexpr int kAdjustFont = 100;
constexpr int kAdjustAutoTurn = 101;
struct NumericRow {
    int option = 0, value = 0, original = 0, minimum = 0, maximum = 0, step = 1; // 手势内的参数快照
    lv_obj_t* label = nullptr; // 数值显示，重建前清理
    bool active = false; // 按住期间不重建控件
    lv_obj_t *unit = nullptr, *decrease = nullptr, *increase = nullptr; // 数值单位和调节按钮
    bool adjustable = true; // EF 字体不允许改变字号
};
static std::vector<NumericRow> s_numeric_rows;
static void UpdateNumericValue(NumericRow& row) {
    char text[24]; std::snprintf(text, sizeof(text), "%d", row.value);
    const char* previous = row.label ? lv_label_get_text(row.label) : nullptr;
    const bool changed = !previous || std::strcmp(previous, text) != 0;
    if (!changed) return;
    lv_label_set_text(row.label, text);
    EpdRequestBlackEqualization();
    if (!row.unit) return;
    auto* box = lv_obj_get_parent(row.label);
    const auto* number_font = SheetUiFont(row.value >= 100 || row.value <= -10 ? 25 : 30, true, text);
    const auto* unit_font = SheetUiFont(14);
    lv_obj_set_style_text_font(row.label, number_font, 0);
    lv_obj_set_style_text_font(row.unit, unit_font, 0);
    lv_obj_set_size(row.label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_size(row.unit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_update_layout(box);
    const int gap = UiSx(2), number_width = lv_obj_get_width(row.label);
    const int left = (lv_obj_get_content_width(box) - number_width - lv_obj_get_width(row.unit) - gap) / 2;
    lv_obj_align(row.label, LV_ALIGN_LEFT_MID, left, 0);
    const int baseline = (number_font->line_height - unit_font->line_height) / 2 - number_font->base_line + unit_font->base_line;
    lv_obj_align(row.unit, LV_ALIGN_LEFT_MID, left + number_width + gap, baseline);
}
static void UpdateNumericBounds(NumericRow& row) {
    if (!row.decrease || !row.increase) return;
    for (auto* button : {row.decrease, row.increase}) {
        const bool enabled = row.adjustable && (button == row.decrease ? row.value > row.minimum : row.value < row.maximum);
        SetSheetBtnEnabled(button, enabled);
        if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}
static const char* SettingsPageTitle(int page) {
    switch (page) {
        case kSettingsRotation: return Lang::Strings::BOOK_ORIENT_TITLE;
        case kSettingsFooter: return Lang::Strings::BOOK_CHAPTER_PROGRESS;
        case kSettingsBackground: return Lang::Strings::BOOK_BACKGROUND_TITLE;
        case kSettingsWriting: return Lang::Strings::BOOK_WRITING_TITLE;
        case kSettingsAntialias: return Lang::Strings::BOOK_ANTIALIAS;
        case kSettingsSpacing: return Lang::Strings::BOOK_SPACING_TITLE;
        case kSettingsUnderline: return Lang::Strings::BOOK_UNDERLINE;
        case kSettingsTurning: return Lang::Strings::BOOK_TURN_TITLE;
        default: return Lang::Strings::BOOK_READING_SETTINGS;
    }
}
static int TextValue(BookTextOption option) {
    const int value = BookReaderPrefsTextValue(option);
    if (value >= 0 || (option != BookTextOption::ParaGap && option != BookTextOption::LineGap && option != BookTextOption::Margin)) return value;
    if (option == BookTextOption::ParaGap) return BookReaderPrefsParaGap();
    if (option == BookTextOption::Margin) return BookReaderPrefsMarginLeft();
    return (BookFont()->line_height * BookReaderPrefsLineSpacingPercent() + 50) / 100;
}
static void RedrawReadingText() {
    if (DeferComicTextSetting()) return;
    CancelReaderPageWork();
    RequestRenderReaderPage();
}
static void CommitNumeric(NumericRow& row) {
    if (!row.active) return;
    row.active = false;
    if (row.value == row.original) return;
    if (row.option == kAdjustFont) {
        if (BookReaderPrefsFontSize() > 0 && PrepareBookFont(BookReaderPrefsFontFile(), row.value)) ScheduleLayoutApply(true);
    } else if (row.option == kAdjustAutoTurn) {
        BookReaderPrefsSetAutoTurnSeconds(row.value);
        ResetReaderAutoTurnDeadline();
    } else {
        BookReaderPrefsSetTextValue(static_cast<BookTextOption>(row.option), row.value);
        if (row.option == static_cast<int>(BookTextOption::UnderlineOffset)) RedrawReadingText();
        else ScheduleLayoutApply(false);
    }
}
static void NumericEvent(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const size_t index = static_cast<size_t>(std::abs(encoded) - 1);
    if (index >= s_numeric_rows.size()) return;
    auto& row = s_numeric_rows[index];
    const auto code = lv_event_get_code(event);
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (code == LV_EVENT_PRESSED) {
        row.original = row.value; row.active = true;
        lv_obj_set_style_bg_color(button, lv_color_black(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_white(), 0);
        EpdRequestBlackEqualization();
    }
    if (row.active && (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT)) {
        row.value = std::clamp(row.value + (encoded > 0 ? row.step : -row.step), row.minimum, row.maximum);
        UpdateNumericValue(row);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_black(), 0);
        CommitNumeric(row);
        UpdateNumericBounds(row);
        EpdRequestBlackEqualization();
    }
}
static void AddNumericRow(lv_obj_t* parent, const char* title, int option, int value,
                           int minimum, int maximum, int step, bool enabled = true, int column = 0, int row_height = 0) {
    lv_obj_update_layout(parent);
    const int width = lv_obj_get_content_width(parent), height = row_height > 0 ? row_height : UiSy(60);
    const int start = title && *title ? (column > 0 ? column : SettingsColumn(parent, {title})) : 0;
    const int gap = UiSx(8);
    const int value_width = std::max<int>(static_cast<int>(UiSx(88)),
        static_cast<int>(Book_MeasureTextWidth(Book_ItemFont(), "-00")) + static_cast<int>(Book_MeasureTextWidth(Book_ItemFont(), "px")) + static_cast<int>(UiSx(16)));
    const int button_width = std::max<int>(1, (static_cast<int>(width) - start - value_width - 2 * static_cast<int>(gap)) / 2);
    auto* row = SheetBox(parent, width, height);
    auto* caption = SheetLabel(row, title);
    if (start > 0) PlaceSettingsCaption(caption, start);
    const int index = static_cast<int>(s_numeric_rows.size());
    auto* dec = SheetButton(row, option == kAdjustFont ? Lang::Strings::BOOK_FONT_DECREASE : Lang::Strings::BOOK_DECREASE, button_width, UiSy(40), NumericEvent, -index - 1, false, UiSx(7));
    auto* val = SheetButton(row, "", value_width, UiSy(40), nullptr, 0, true, UiSx(7));
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dec, start, (height - UiSy(40)) / 2);
    lv_obj_set_pos(val, start + button_width + gap, (height - UiSy(40)) / 2);
    auto* inc = SheetButton(row, option == kAdjustFont ? Lang::Strings::BOOK_FONT_INCREASE : Lang::Strings::BOOK_INCREASE, button_width, UiSy(40), NumericEvent, index + 1, false, UiSx(7));
    lv_obj_set_pos(inc, start + button_width + value_width + 2 * gap, (height - UiSy(40)) / 2);
    auto* label = lv_obj_get_child(val, 0);
    auto* unit = SheetLabel(val, option == kAdjustAutoTurn ? "s" : "px", SheetUiFont(14));
    lv_obj_set_style_text_color(unit, lv_color_white(), 0);
    s_numeric_rows.push_back({option, value, value, minimum, maximum, step, label, false, unit, dec, inc, enabled});
    UpdateNumericValue(s_numeric_rows.back());
    for (auto* button : {dec, inc}) {
        lv_obj_remove_event_cb(button, NumericEvent);
        const intptr_t binding = button == dec ? -index - 1 : index + 1;
        lv_obj_add_event_cb(button, NumericEvent, LV_EVENT_ALL, reinterpret_cast<void*>(binding));
        lv_obj_set_style_border_width(button, 0, LV_STATE_DISABLED);
        lv_obj_add_event_cb(button, DisabledAdjustmentDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
    }
    UpdateNumericBounds(s_numeric_rows.back());
}
static void SettingsNavigate(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    Book_State().settings.reader_settings_page = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    Book_State().settings.reader_scroll[3] = 0;
    RebuildSettingsSheet();
}
static void SettingsBack(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (Book_State().settings.reader_tab == 3 && Book_State().settings.reader_settings_page != kSettingsHome) {
        Book_State().settings.reader_settings_page = kSettingsHome;
        Book_State().settings.reader_scroll[3] = 0;
        RebuildSettingsSheet();
    } else SheetClose(event);
}
static void SettingsReset(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (Book_State().settings.reader_tab == 1) {
        if (PrepareBookFont(kBookReaderDefaultFontFile, 0)) ScheduleLayoutApply(true);
    } else {
        SetReaderAutoTurnEnabled(false);
        BookTapZonesResetDefaults();
        BookReaderPrefsResetReading();
        EpdSetReaderEffect(EpdEffect::None);
        SyncReaderAutoOrient();
        if (DisplayUiGetOrient() != kBookReaderOrientPortrait) ApplyReaderOrient(kBookReaderOrientPortrait);
        else ScheduleLayoutApply(false);
        SyncFooterClockTimer();
    }
    RequestSettingsSheetPaint();
}
static void TextChoice(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const int encoded = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    BookReaderPrefsSetTextValue(encoded >= 10 ? BookTextOption::AaStrength : BookTextOption::Indent, encoded % 10);
    RequestSettingsSheetPaint();
    if (encoded >= 10) RedrawReadingText();
    else ScheduleLayoutApply(false);
}
static void SheetAutoTurnToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    SetReaderAutoTurnEnabled(!ReaderAutoTurnEnabled());
    RequestSettingsSheetPaint();
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
    if (mode == 3) {
        OpenReadingWallpaper();
        return;
    }
    BookReaderPrefsSetBackground(BookReaderPrefsBackground() == mode ? 0 : mode);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    CancelReaderPageWork();
    RequestRenderReaderPage();
}
static void SheetAntialias(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int enabled = !BookReaderPrefsAntialias();
    BookReaderPrefsSetAntialias(enabled);
    RequestSettingsSheetPaint();
    if (DeferComicTextSetting()) return;
    CancelReaderPageWork();
    RequestRenderReaderPage();
}
struct SwitchRow { lv_obj_t* button = nullptr; lv_obj_t* knob = nullptr; lv_event_cb_t event = nullptr; int button_w = 0; int displayed = -1; };
static std::vector<SwitchRow> s_switch_rows;
static void PlaceSwitchCaption(lv_obj_t* caption, int button_w) {
    const int toggle_space = static_cast<int>(UiSx(8)) + static_cast<int>(UiSx(34)) + static_cast<int>(UiSx(8));
    const int text_w = std::max<int>(1, button_w - toggle_space);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(caption, text_w);
    lv_obj_set_height(caption, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, 0);
}
static void RefreshSwitchRows() {
    for (auto& row : s_switch_rows) {
        const bool on = row.event == SheetAntialias ? BookReaderPrefsAntialias() : ReaderAutoTurnEnabled();
        auto* caption = lv_obj_get_child(row.button, 0);
        const char* text = on ? Lang::Strings::COMMON_ON : Lang::Strings::COMMON_OFF;
        if (row.displayed == static_cast<int>(on)) continue;
        row.displayed = on;
        lv_label_set_text(caption, text);
        PlaceSwitchCaption(caption, row.button_w);
        lv_obj_set_style_bg_color(row.button, lv_color_white(), 0);
        lv_obj_set_style_text_color(caption, lv_color_black(), 0);
        auto* track = lv_obj_get_parent(row.knob);
        lv_obj_set_style_bg_color(track, on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_color(row.knob, on ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_border_color(track, lv_color_black(), 0);
        lv_obj_align(row.knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        lv_obj_move_foreground(track);
        if (on) EpdRequestBlackEqualization();
    }
}
static void AddSwitchRow(lv_obj_t* body, const char* title, lv_event_cb_t event, int column = 0) {
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body), height = UiSy(60);
    const int start = column > 0 ? column : SettingsColumn(body, {title});
    const int button_w = std::max<int>(1, static_cast<int>(width) - start);
    const bool on = event == SheetAntialias ? BookReaderPrefsAntialias() : ReaderAutoTurnEnabled();
    auto* row = SheetBox(body, width, height);
    auto* label = SheetLabel(row, title);
    PlaceSettingsCaption(label, start);
    auto* button = SheetButton(row, on ? Lang::Strings::COMMON_ON : Lang::Strings::COMMON_OFF,
                               button_w, UiSy(40), event, 0, false, UiSx(7));
    lv_obj_align(button, LV_ALIGN_LEFT_MID, start, 0);
    PlaceSwitchCaption(lv_obj_get_child(button, 0), button_w);
    auto* track = SheetBox(button, UiSx(34), UiSx(19));
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, -UiSx(8), 0);
    auto* knob = SheetBox(track, UiSx(14), UiSx(14));
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_y(knob, UiSy(2));
    s_switch_rows.push_back({button, knob, event, button_w});
    RefreshSwitchRows();
}
static void BuildSheetSettings(lv_obj_t* body) {
    using namespace Lang::Strings;
    auto& settings = Book_State().settings;
    const int page = settings.reader_settings_page;
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body);
    if (page == kSettingsHome) {
        const int pages[] = {kSettingsRotation, kSettingsFooter, -1, kSettingsBackground,
            kSettingsWriting, kSettingsAntialias, kSettingsSpacing, kSettingsUnderline, kSettingsTurning};
        const char* titles[] = {BOOK_ORIENT_TITLE, BOOK_CHAPTER_PROGRESS, BOOK_TAP_ZONES_TITLE,
            BOOK_BACKGROUND_TITLE, BOOK_WRITING_TITLE, BOOK_ANTIALIAS, BOOK_SPACING_TITLE, BOOK_UNDERLINE, BOOK_TURN_TITLE};
        const char* orientation = BookReaderPrefsOrient() == kBookReaderOrientAuto ? BOOK_ORIENT_AUTO :
            BookReaderPrefsOrient() == kBookReaderOrientLandLeft ? BOOK_ORIENT_LAND_LEFT :
            BookReaderPrefsOrient() == kBookReaderOrientLandRight ? BOOK_ORIENT_LAND_RIGHT : BOOK_ORIENT_PORTRAIT;
        const int bg = BookReaderPrefsBackground();
        const char* values[] = {orientation, BOOK_FOOTER_BOTTOM, BookTapZonesIsDefault() ? BOOK_DEFAULT : BOOK_CUSTOM,
            bg == 0 ? COMMON_OFF : bg == 1 ? BOOK_BACKGROUND_GRID : bg == 2 ? BOOK_BACKGROUND_DOTS : BOOK_CUSTOM,
            BookReaderPrefsWritingMode() ? BOOK_WRITING_VERTICAL : BOOK_WRITING_HORIZONTAL,
            BookReaderPrefsAntialias() ? (TextValue(BookTextOption::AaStrength) == 0 ? BOOK_AA_LOW : TextValue(BookTextOption::AaStrength) == 1 ? BOOK_AA_MEDIUM : BOOK_AA_HIGH) : COMMON_OFF,
            BOOK_CUSTOM, BookReaderPrefsUnderlineMode() == 0 ? BOOK_TAP_ZONE_NONE : BookReaderPrefsUnderlineMode() == 1 ? BOOK_UNDERLINE_SOLID : BOOK_UNDERLINE_DASHED,
            ReaderAutoTurnEnabled() ? COMMON_ON : COMMON_OFF};
        const int gap = UiSx(10), card_w = (width - 2 * gap) / 3, card_h = UiSy(82);
        lv_obj_set_layout(body, LV_LAYOUT_NONE);
        for (int i = 0; i < 9; ++i) {
            auto* card = SheetButton(body, "", card_w, card_h, pages[i] < 0 ? OnSheetTapZonesClicked : SettingsNavigate, pages[i], false, UiSx(10));
            lv_obj_set_pos(card, (i % 3) * (card_w + gap), (i / 3) * (card_h + UiSy(9)));
            auto* title = lv_obj_get_child(card, 0);
            lv_label_set_text(title, titles[i]);
            ScreenFitLabel(title, card_w - UiSx(20), card_h / 3, SheetUiFont(16, false, titles[i]));
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, UiSx(10), UiSy(11));
            auto* value = SheetLabel(card, values[i], SheetUiFont(20, true, values[i]));
            ScreenFitLabel(value, card_w - UiSx(30), card_h / 3, SheetUiFont(20, true, values[i]));
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, UiSx(10), -UiSy(10));
            auto* value_bold = SheetLabel(card, values[i], lv_obj_get_style_text_font(value, LV_PART_MAIN));
            lv_obj_set_width(value_bold, card_w - UiSx(30));
            lv_label_set_long_mode(value_bold, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(value_bold, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(value_bold, LV_ALIGN_BOTTOM_LEFT, UiSx(10) + 1, -UiSy(10));
            auto* arrow = SheetLabel(card, ">");
            lv_obj_align(arrow, LV_ALIGN_BOTTOM_RIGHT, -UiSx(8), -UiSy(8));
        }
        return;
    }
    ConfigureSheetScroll(body);
    if (page == kSettingsRotation || page == kSettingsBackground) {
        const bool background = page == kSettingsBackground;
        const char* orientation_labels[] = {BOOK_ORIENT_PORTRAIT, BOOK_ORIENT_LAND_LEFT, BOOK_ORIENT_LAND_RIGHT, BOOK_ORIENT_AUTO};
        const int orientation_values[] = {kBookReaderOrientPortrait, kBookReaderOrientLandLeft, kBookReaderOrientLandRight, kBookReaderOrientAuto};
        const char* background_labels[] = {COMMON_OFF, BOOK_BACKGROUND_GRID, BOOK_BACKGROUND_DOTS, BOOK_CUSTOM};
        const int background_values[] = {0, 1, 2, 3};
        const auto* labels = background ? background_labels : orientation_labels;
        const auto* values = background ? background_values : orientation_values;
        const auto event = background ? SheetBackground : OnSheetOrientMode;
        const int selected = background ? BookReaderPrefsBackground() : BookReaderPrefsOrient();
        const int gap = UiSx(8), w = (width - 3 * gap) / 4;
        auto* row = SheetBox(body, width, UiSy(170));
        for (int i = 0; i < 4; ++i) {
            auto* button = SheetButton(row, labels[i], w, UiSy(150), event, values[i], selected == values[i], UiSx(7));
            lv_obj_set_pos(button, i * (w + gap), UiSy(10));
            s_sheet_bindings.push_back({button, event, values[i], false});
        }
    } else if (page == kSettingsFooter) {
        const int column = SettingsColumn(body, {BOOK_CHAPTER_PROGRESS, BOOK_FOOTER_LAYOUT});
        const char* fields[] = {BOOK_FOOTER_TITLE, BOOK_FOOTER_TIME, BOOK_FOOTER_PROGRESS, BOOK_FOOTER_BATTERY};
        const int bits[] = {kBookReaderFooterTitle, kBookReaderFooterTime, kBookReaderFooterProgress, kBookReaderFooterBattery};
        SettingsRow(body, BOOK_CHAPTER_PROGRESS, 4, fields, OnSheetFooterBitToggle, bits, BookReaderPrefsFooterMask(), true, 0, column);
        const char* layouts[] = {BOOK_FOOTER_BOTTOM}; const int values[] = {0};
        SettingsRow(body, BOOK_FOOTER_LAYOUT, 1, layouts, nullptr, values, 0, false, 0, column);
    } else if (page == kSettingsWriting) {
        const int column = SettingsColumn(body, {BOOK_WRITING_TITLE, BOOK_TEXT_DIRECTION});
        const char* labels[] = {BOOK_WRITING_HORIZONTAL, BOOK_WRITING_VERTICAL}; const int values[] = {0, 1};
        SettingsRow(body, BOOK_WRITING_TITLE, 2, labels, SheetWritingMode, values, BookReaderPrefsWritingMode(), false, 0, column);
        if (BookReaderPrefsWritingMode()) {
            const char* directions[] = {BOOK_TEXT_LEFT_TO_RIGHT, BOOK_TEXT_RIGHT_TO_LEFT};
            SettingsRow(body, BOOK_TEXT_DIRECTION, 2, directions, SheetTextDirection, values, BookReaderPrefsTextDirection(), false, 0, column);
        }
    } else if (page == kSettingsAntialias) {
        const int column = SettingsColumn(body, {BOOK_ANTIALIAS, BOOK_AA_STRENGTH});
        AddSwitchRow(body, BOOK_ANTIALIAS, SheetAntialias, column);
        const char* strengths[] = {BOOK_AA_LOW, BOOK_AA_MEDIUM, BOOK_AA_HIGH}; const int levels[] = {10, 11, 12};
        SettingsRow(body, BOOK_AA_STRENGTH, 3, strengths, TextChoice, levels, 10 + TextValue(BookTextOption::AaStrength), false, 0, column);
    } else if (page == kSettingsSpacing) {
        const int row_height = LV_HOR_RES > LV_VER_RES ? std::min<int>(UiSy(52), lv_obj_get_content_height(body) / 5) : UiSy(60);
        const int column = SettingsColumn(body, {BOOK_PARA_GAP_TITLE, BOOK_LETTER_GAP, BOOK_LINE_GAP, BOOK_MARGIN, BOOK_INDENT});
        AddNumericRow(body, BOOK_PARA_GAP_TITLE, static_cast<int>(BookTextOption::ParaGap), TextValue(BookTextOption::ParaGap), 0, 104, 2, true, column, row_height);
        AddNumericRow(body, BOOK_LETTER_GAP, static_cast<int>(BookTextOption::LetterGap), TextValue(BookTextOption::LetterGap), -2, 12, 1, true, column, row_height);
        AddNumericRow(body, BOOK_LINE_GAP, static_cast<int>(BookTextOption::LineGap), TextValue(BookTextOption::LineGap), 0, 48, 1, true, column, row_height);
        AddNumericRow(body, BOOK_MARGIN, static_cast<int>(BookTextOption::Margin), TextValue(BookTextOption::Margin), 0, 64, 2, true, column, row_height);
        const char* labels[] = {BOOK_TAP_ZONE_NONE, BOOK_INDENT_ONE, BOOK_INDENT_TWO, BOOK_INDENT_THREE}; const int values[] = {0, 1, 2, 3};
        SettingsRow(body, BOOK_INDENT, 4, labels, TextChoice, values, TextValue(BookTextOption::Indent), false, 0, column, row_height);
    } else if (page == kSettingsUnderline) {
        const int column = SettingsColumn(body, {BOOK_UNDERLINE, BOOK_UNDERLINE_POSITION});
        const char* labels[] = {BOOK_TAP_ZONE_NONE, BOOK_UNDERLINE_SOLID, BOOK_UNDERLINE_DASHED}; const int values[] = {0, 1, 2};
        SettingsRow(body, BOOK_UNDERLINE, 3, labels, OnSheetUnderlineMode, values, BookReaderPrefsUnderlineMode(), false, 0, column);
        AddNumericRow(body, BOOK_UNDERLINE_POSITION, static_cast<int>(BookTextOption::UnderlineOffset),
                      TextValue(BookTextOption::UnderlineOffset), -12, 12, 2, true, column);
    } else if (page == kSettingsTurning) {
        const int column = SettingsColumn(body, {BOOK_TURN_TITLE, BOOK_AUTO_TURN, BOOK_TURN_INTERVAL});
        const char* labels[] = {BOOK_TURN_SLIDE, BOOK_TURN_PUSH}; const int values[] = {kBookReaderPageTurnSlide, kBookReaderPageTurnWipe};
        SettingsRow(body, BOOK_TURN_TITLE, 2, labels, SheetPageTurn, values, BookReaderPrefsPageTurn(), false, 0, column);
        AddSwitchRow(body, BOOK_AUTO_TURN, SheetAutoTurnToggle, column);
        AddNumericRow(body, BOOK_TURN_INTERVAL, kAdjustAutoTurn, BookReaderPrefsAutoTurnSeconds(), 5, 45, 1, true, column);
    }
}
static int SettingsBodyHeight(int page) {
    const int pad = UiSy(4) * 2;
    const int row = UiSy(60);
    int content = row;
    switch (page) {
        case kSettingsHome:
            content = 3 * UiSy(82) + 2 * UiSy(9);
            break;
        case kSettingsRotation:
        case kSettingsBackground:
            content = UiSy(170);
            break;
        case kSettingsFooter:
        case kSettingsAntialias:
        case kSettingsUnderline:
            content = row * 2;
            break;
        case kSettingsWriting:
            content = row * (BookReaderPrefsWritingMode() ? 2 : 1);
            break;
        case kSettingsSpacing:
            content = (LV_HOR_RES > LV_VER_RES ? UiSy(52) : row) * 5;
            break;
        case kSettingsTurning:
            content = row * 3;
            break;
        default:
            content = row * 3;
            break;
    }
    return content + pad;
}
// Only viewport rows exist; a one-pixel extent preserves the full scroll range.
static lv_obj_t* s_toc_list = nullptr;
static std::vector<lv_obj_t*> s_toc_rows;
constexpr int kTocPageRows = 9;
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
    if(list==s_toc_list)more=current+lv_obj_get_height(list)<s_toc_items*s_toc_height;
    auto* previous=lv_obj_get_child(holder,1);auto* next=lv_obj_get_child(holder,2);
    if(current>0)lv_obj_remove_state(previous,LV_STATE_DISABLED);else lv_obj_add_state(previous,LV_STATE_DISABLED);
    if(more)lv_obj_remove_state(next,LV_STATE_DISABLED);else lv_obj_add_state(next,LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(lv_obj_get_child(previous,0),current>0?LV_OPA_COVER:LV_OPA_40,0);
    lv_obj_set_style_text_opa(lv_obj_get_child(next,0),more?LV_OPA_COVER:LV_OPA_40,0);
}
struct SheetPageGesture {
    lv_obj_t* list = nullptr;
    int stride = 1;
    int pages = 1;
};
static SheetPageGesture s_font_pages, s_toc_pages;
static void ChangeSheetListPage(SheetPageGesture& pages, int direction) {
    if (!pages.list || pages.pages <= 1) return;
    const int current = lv_obj_get_scroll_y(pages.list) / pages.stride;
    const int target = std::clamp(current + direction, 0, pages.pages - 1);
    if (target == current) return;
    lv_obj_scroll_to_y(pages.list, target * pages.stride, LV_ANIM_OFF);
    auto& settings = Book_State().settings;
    settings.reader_scroll[settings.reader_tab] = lv_obj_get_scroll_y(pages.list);
}
static void SheetListGesture(lv_event_t* event) {
    auto* pages = static_cast<SheetPageGesture*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (!pages || !indev) return;
    const auto direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM) return;
    lv_event_stop_bubbling(event);
    lv_indev_wait_release(indev);
    ChangeSheetListPage(*pages, direction == LV_DIR_TOP ? 1 : -1);
}
static void DrawSheetPageIndicator(lv_event_t* event) {
    const auto* pages = static_cast<const SheetPageGesture*>(lv_event_get_user_data(event));
    if (!pages || pages->pages <= 1) return;
    lv_area_t area;
    lv_obj_get_coords(pages->list, &area);
    const int inset = UiSy(4);
    const int track = std::max<int>(1, lv_area_get_height(&area) - 2 * inset);
    const int length = std::min(track, std::max<int>(UiSy(12), track / pages->pages));
    const int page = std::clamp<int>(lv_obj_get_scroll_y(pages->list) / pages->stride, 0, pages->pages - 1);
    area.y1 += inset + (track - length) * page / (pages->pages - 1);
    area.y2 = area.y1 + length - 1;
    area.x2 -= 1;
    area.x1 = area.x2 - UiSx(5) + 1;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = lv_color_black();
    bar.bg_opa = LV_OPA_COVER;
    bar.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(lv_event_get_layer(event), &bar, &area);
}
static void ConfigureSheetPaging(lv_obj_t* list, SheetPageGesture& pages, int stride, int count) {
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
    }
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
        ChangeSheetListPage(s_toc_pages, reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) > 0 ? 1 : -1);
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
    lv_obj_add_event_cb(list,SheetListPageState,LV_EVENT_SCROLL,nullptr);
    // Extra bottom padding allows the last partially visible item to become page start.
    lv_obj_set_style_pad_bottom(list,std::max(0,height-nav-4),0);
    return list;
}
static void BuildSheetFonts(lv_obj_t* body) {
    auto& st = Book_State();
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body), gap = UiSx(10);
    const int adjust_height = st.settings.font_multi ? 0 : UiSy(55);
    const int height = std::max<int>(UiSy(90), lv_obj_get_content_height(body) - adjust_height - UiSy(10) - 4);
    auto* frame = SheetBox(body, width, height);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_post(frame, true, 0);
    lv_obj_set_style_radius(frame, UiSx(8), 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    auto* list = SheetBox(frame, width - 4, height - 8);
    lv_obj_set_style_pad_all(list, gap, 0);
    ConfigureSheetScroll(list);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_update_layout(list);
    const int card_w = (lv_obj_get_content_width(list) - 2 * gap) / 3;
    const int card_h = std::max<int>(UiSy(28), (lv_obj_get_content_height(list) - 2 * gap) / 3);
    if (st.settings.font_scan_loading || st.settings.font_scan_failed || st.settings.font_entries.empty()) {
        Book_DisableScroll(list);
        Book_ShowMessage(list, st.settings.font_scan_loading ? Lang::Strings::BOOK_FONT_LOADING :
            st.settings.font_scan_failed ? Lang::Strings::BOOK_FONT_REOPEN : Lang::Strings::BOOK_FONT_EMPTY, Book_ItemFont());
    } else {
        lv_obj_t* current = nullptr;
        for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
            const auto& entry = st.settings.font_entries[i];
            const bool active = st.settings.font_multi ? FontItemSelected(i) : entry.file == BookReaderPrefsFontFile();
            auto* card = SheetButton(list, "", card_w, card_h, OnSheetFontSelect, 0, active, UiSx(7));
            lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(card, OnSheetFontLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
            lv_obj_set_pos(card, (i % 3) * (card_w + gap), (i / 3) * (card_h + gap));
            if (i % 9 == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_SNAPPABLE);
            else lv_obj_remove_flag(card, LV_OBJ_FLAG_SNAPPABLE);
            auto* label = lv_obj_get_child(card, 0);
            char name[SD_FILE_NAME_CAPACITY]; FormatFontListName(entry.file, name, sizeof(name));
            lv_label_set_text(label, name);
            lv_obj_set_width(label, card_w - UiSx(12));
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            ScreenFitLabel(label, card_w - UiSx(12), card_h / 2, SheetUiFont(20, false, name));
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_height(label, lv_obj_get_style_text_font(label, LV_PART_MAIN)->line_height);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, -card_h / 6);
            char meta[32];
            if (IsTtfFileName(entry.file.c_str())) std::snprintf(meta, sizeof(meta), "%s%s", strcasecmp(entry.file.c_str() + entry.file.size() - 4, ".otf") == 0 ? "OTF" : "TTF", st.settings.font_multi && active ? " *" : "");
            else std::snprintf(meta, sizeof(meta), "EF %dpx%s", entry.size_px, st.settings.font_multi && active ? " *" : "");
            auto* info = SheetLabel(card, meta, SheetUiFont(14));
            lv_obj_set_style_text_color(info, active ? lv_color_white() : lv_color_black(), 0);
            ScreenFitLabel(info, card_w - UiSx(12), card_h / 3, SheetUiFont(14));
            lv_obj_align(info, LV_ALIGN_CENTER, 0, card_h / 4);
            if (!st.settings.font_multi && active) current = card;
        }
        if (st.settings.reader_scroll[1] < 0) st.settings.reader_scroll[1] = current ? lv_obj_get_y(current) / (3 * (card_h + gap)) * (3 * (card_h + gap)) : 0;
        const int pages = (static_cast<int>(st.settings.font_entries.size()) + 8) / 9;
        const int stride = 3 * (card_h + gap);
        auto* extent = SheetBox(list, 1, 1);
        lv_obj_set_pos(extent, 0, (pages - 1) * stride + lv_obj_get_content_height(list) - 1);
        ConfigureSheetPaging(list, s_font_pages, stride, pages);
    }
    if (!st.settings.font_multi) {
        const bool vector = BookReaderPrefsFontSize() > 0;
        const int index = CurrentFontIndex();
        const int size = vector ? BookReaderPrefsFontSize() : index >= 0 ? st.settings.font_entries[index].size_px : BookFont()->line_height;
        AddNumericRow(body, "", kAdjustFont, size, 20, 80, 1, vector);
    }
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
    s_toc_home=session.TocStartsAfterFirstPage();
    const int chapters_per_page = kTocPageRows - (s_toc_home ? 1 : 0);
    const int home_rows = s_toc_home ? (s_toc_count + chapters_per_page - 1) / chapters_per_page : 0;
    s_toc_items = s_toc_count + home_rows;
    s_toc_building=session.LiveRelayoutPending();
    s_toc_first=-1;
    lv_obj_set_layout(list,LV_LAYOUT_NONE);
    lv_obj_set_style_pad_bottom(list,0,0);
    lv_obj_update_layout(list);
    s_toc_height = std::max<int>(1, lv_obj_get_content_height(list) / kTocPageRows);
    if (!s_toc_count) {
        Book_ShowMessage(list, session.LiveRelayoutPending() ?
            Lang::Strings::BOOK_TOC_BUILDING : Lang::Strings::BOOK_TOC_EMPTY, Book_ItemFont());
        return;
    }
    const int viewport = kTocPageRows * s_toc_height;
    lv_obj_set_height(list, viewport);
    auto* extent=SheetBox(list,1,1);
    lv_obj_set_pos(extent,0,(s_toc_items-1)*s_toc_height+viewport-1);
    const int pool=std::min(s_toc_items,(viewport+s_toc_height-1)/s_toc_height+2);
    for(int slot=0;slot<pool;++slot) {
        auto* row=SheetButton(list,"",width,s_toc_height,SheetTocJump,0);
        for(int label=0;label<3;++label)SheetLabel(row,"");
        s_toc_rows.push_back(row);
    }
    if(st.settings.reader_scroll[0]<0) {
        const int current=std::max(0,session.CurrentTocIndex());
        const int display = s_toc_home ? current / chapters_per_page * kTocPageRows : current;
        st.settings.reader_scroll[0] = display * s_toc_height;
    }
    const int rows = std::max(1, viewport / s_toc_height);
    ConfigureSheetPaging(list, s_toc_pages, rows * s_toc_height, (s_toc_items + rows - 1) / rows);
    lv_obj_add_event_cb(list,TocScroll,LV_EVENT_SCROLL,nullptr);
    BindTocRows();

}

static void BindTocRows() {
    if(!s_toc_list || s_toc_binding || !s_toc_count)return;
    const int first=std::clamp<int>(lv_obj_get_scroll_y(s_toc_list)/s_toc_height,0,std::max(0,s_toc_items-int(s_toc_rows.size())));
    if(first==s_toc_first)return;
    s_toc_binding=true;s_toc_first=first;
    auto& session=*Book_State().reader.session;
    const int width=lv_obj_get_width(s_toc_list);
    const int current_chapter = session.CurrentTocIndex();
    const bool at_home = s_toc_home && session.BookCurrentPage() == 0;
    const char* home_title = s_toc_home ? session.TocHomeTitle() : nullptr;
    for(size_t slot=0;slot<s_toc_rows.size();++slot) {
        const int display = first + static_cast<int>(slot);
        const int page = display / kTocPageRows;
        const int page_row = display % kTocPageRows;
        const bool home = s_toc_home && page_row == 0;
        const int i = s_toc_home ? page * (kTocPageRows - 1) + page_row - 1 : display;
        const auto* entry = home ? nullptr : session.TocAt(i);
        if (!home && !entry) continue;
        const char* title = home ? home_title : entry->title.c_str();
        std::string ordinal, name;
        const bool split = !home && reader::SplitChapterHeading(entry->title, ordinal, name);
        const bool current = home ? at_home : !at_home && i == current_chapter;
        const auto* font = current ? Book_ListFont() : Book_ItemFont();
        const int ordinal_w = split ? std::min(width / 2, std::max<int>(UiSx(90), Book_MeasureTextWidth(font, ordinal.c_str()) + UiSx(12))) : 0;
        const int text_w = width - ordinal_w - UiSx(16);
        auto* row=s_toc_rows[slot];
        lv_obj_remove_flag(row,LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(row,display*s_toc_height);
        lv_obj_remove_event_cb(row,SheetTocJump);
        const int target = home ? kSheetProgressBase : i;
        lv_obj_add_event_cb(row, SheetTocJump, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<intptr_t>(target)));
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        auto* text = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_font(text, font, 0);
        lv_obj_set_size(text, text_w, font->line_height);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_label_set_text(text, split ? name.c_str() : title);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(text, LV_ALIGN_LEFT_MID, ordinal_w + UiSx(8), 0);
        for(int label=1;label<4;++label)lv_obj_add_flag(lv_obj_get_child(row,label),LV_OBJ_FLAG_HIDDEN);
        if (current) {
            auto* bold = lv_obj_get_child(row,1);
            lv_obj_remove_flag(bold,LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(bold,font,0);
            lv_label_set_text(bold,split?name.c_str():title);
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
            if (current) {
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
    lv_obj_update_layout(body);
    const int width = lv_obj_get_content_width(body);
    auto* bookmark = SheetButton(body, "", UiSx(34), UiSy(34), BookmarkActionEvent, kBookmarkAdd, session.CurrentPageBookmarkId() != 0);
    lv_obj_add_flag(bookmark, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(bookmark, LV_ALIGN_TOP_RIGHT, -UiSx(4), 6);
    lv_obj_set_style_radius(bookmark, LV_RADIUS_CIRCLE, 0);
    s_bookmark_button = bookmark;
    s_bookmark_icon = lv_image_create(bookmark);
    lv_image_set_src(s_bookmark_icon, SheetBookmarkIcon());
    lv_obj_set_style_image_recolor_opa(s_bookmark_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(s_bookmark_icon, session.CurrentPageBookmarkId() ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(s_bookmark_icon);
    lv_obj_clear_flag(s_bookmark_icon, LV_OBJ_FLAG_CLICKABLE);
    auto* row = SheetBox(body, width, UiSy(80));
    const uint32_t seconds = session.ReadingSeconds();
    char duration[64]; std::snprintf(duration, sizeof(duration), Lang::Strings::BOOK_DURATION_HM_FMT, seconds / 3600, seconds % 3600 / 60);
    char progress[64]; const int pct = session.ReadingProgressX10();
    std::snprintf(progress, sizeof(progress), "%.2f%%",
        session.ReadingProgressX10() / 10.0);
    const char* titles[] = {Lang::Strings::BOOK_READ_DURATION, Lang::Strings::BOOK_READING_PROGRESS};
    const char* values[] = {duration, progress};
    for (int i = 0; i < 2; ++i) {
        auto* column = SheetBox(row, width / 2, UiSy(80));
        lv_obj_set_pos(column, i * width / 2, 0);
        auto* title = SheetLabel(column, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UiSy(22));
        auto* value = SheetLabel(column, values[i], SheetUiFont(30, true, values[i]));
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
    for (auto& row : s_numeric_rows) CommitNumeric(row);
    s_font_pages = {}; s_toc_pages = {};
    s_numeric_rows.clear();
    s_switch_rows.clear();
    s_toc_list=nullptr;s_toc_rows.clear();s_toc_count=0;s_toc_items=0;s_toc_home=false;s_toc_building=false;s_toc_first=-1;
    s_sheet_bindings.clear();s_live_body=nullptr;s_live_tab=-1;
    s_bookmark_button=s_bookmark_icon=s_duration_value=s_progress_value=s_progress_slider=s_chapter_prev=s_chapter_next=nullptr;
}
static void SetLiveSheetButton(lv_obj_t* button,bool active) {
    const bool was_active = lv_color_eq(lv_obj_get_style_bg_color(button, LV_PART_MAIN), lv_color_black());
    if (was_active == active) return;
    lv_obj_set_style_bg_color(button,active?lv_color_black():lv_color_white(),0);
    auto* label=lv_obj_get_child(button,0);
    if(label)lv_obj_set_style_text_color(label,active?lv_color_white():lv_color_black(),0);
    SyncButtonEmphasis(button, active);
    if (active) EpdRequestBlackEqualization();
}
bool RefreshReadingSheetInPlace() {
    auto& st=Book_State();
    if(!s_live_body || s_live_body!=st.settings.reader_sheet_body || !lv_obj_is_valid(s_live_body) ||
       s_live_tab!=st.settings.reader_tab ||
       s_live_width!=LV_HOR_RES || s_live_height!=LV_VER_RES ||
       (s_live_tab!=0 && s_live_tab!=3 && s_live_tab!=kDurationTab))return false;
    if (s_live_tab == 3 && s_live_settings_page != st.settings.reader_settings_page) return false;
    if (s_live_tab == 3 && (st.settings.reader_settings_page == kSettingsHome || st.settings.reader_settings_page == kSettingsWriting)) return false;
    if(s_live_tab==0) {
        if(!st.reader.session || !s_toc_count || st.reader.session->TocCount()!=s_toc_count ||
            st.reader.session->TocStartsAfterFirstPage()!=s_toc_home)return false;
        s_toc_first=-1;
        BindTocRows();
        return true;
    }
    RefreshSwitchRows();
    for (auto& row : s_numeric_rows) {
        if (row.active) continue;
        if (row.option < static_cast<int>(BookTextOption::Count)) row.value = TextValue(static_cast<BookTextOption>(row.option));
        UpdateNumericValue(row);
        UpdateNumericBounds(row);
    }
    for(const auto& binding:s_sheet_bindings) {
        int selected=-1;
        if(binding.event==OnSheetOrientMode)selected=BookReaderPrefsOrient();
        else if(binding.event==SheetAutoTurnToggle)selected=ReaderAutoTurnEnabled()?1:0;
        else if(binding.event==SheetWritingMode)selected=BookReaderPrefsWritingMode();
        else if(binding.event==SheetTextDirection)selected=BookReaderPrefsTextDirection();
        else if(binding.event==SheetPageTurn)selected=BookReaderPrefsPageTurn();
        else if(binding.event==SheetBackground)selected=BookReaderPrefsBackground();
        else if(binding.event==SheetAntialias)selected=BookReaderPrefsAntialias();
        else if(binding.event==OnSheetFooterBitToggle)selected=BookReaderPrefsFooterMask();
        else if(binding.event==OnSheetUnderlineMode)selected=BookReaderPrefsUnderlineMode();
        else if(binding.event==TextChoice)selected=binding.value>=10?10+TextValue(BookTextOption::AaStrength):TextValue(BookTextOption::Indent);
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
    if (!panel || !lv_obj_is_valid(panel)) return;
    lv_obj_set_style_radius(panel, UiSx(12), 0);
    lv_obj_set_style_border_width(panel, kReadingCardBorderWidth, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_side(panel, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_border_post(panel, true, 0);
    lv_obj_update_layout(panel);
    static int nav_rule_tag;
    lv_obj_t* rule = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(panel); ++i) {
        auto* child = lv_obj_get_child(panel, i);
        if (lv_obj_get_user_data(child) == &nav_rule_tag) { rule = child; break; }
    }
    if (!rule) {
        rule = SheetBox(panel, 1, kReadingCardBorderWidth);
        lv_obj_set_user_data(rule, &nav_rule_tag);
        lv_obj_add_flag(rule, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    }
    lv_obj_set_size(rule, lv_obj_get_content_width(panel), kReadingCardBorderWidth);
    lv_obj_set_pos(rule, 0, lv_obj_get_height(panel) - bottom_gap - kReadingCardBorderWidth -
        lv_obj_get_style_pad_top(panel, LV_PART_MAIN) - kReadingCardBorderWidth);
    lv_obj_move_foreground(rule);
}
int ReadingSheetNavReserve() {
    return SheetNavHeight() + 4;
}
void ResizeReadingBottomSheet() {
    auto& st = Book_State();auto* sheet = st.settings.settings_sheet;
    if (!sheet || !lv_obj_is_valid(sheet)) return;
    const bool compact = st.settings.reader_tab == kDurationTab;
    const bool full = st.settings.reader_tab == 0 || st.settings.reader_tab == 2;
    const int header_h = std::min<int>(UiSy(56), LV_VER_RES / 8);
    const int nav_h = ReadingSheetNavReserve();
    int desired = compact ? UiSy(220) :
        full ? LV_VER_RES :
        st.settings.reader_tab == 3 ? header_h + SettingsBodyHeight(st.settings.reader_settings_page) + nav_h : UiSy(430);
    if (st.settings.reader_tab == 1) desired = UiSy(440);
    const int limit = compact ? LV_VER_RES * 3 / 5 : full || st.settings.reader_tab == 1
        ? LV_VER_RES : LV_VER_RES - (st.reader.status_h > 0 ? st.reader.status_h : 40);
    const int status_h = st.reader.status_h > 0 ? st.reader.status_h : 40;
    const int available = std::max<int>(1, LV_VER_RES - status_h);
    lv_obj_set_size(sheet, std::max<int>(1, LV_HOR_RES - 2 * kReadingCardSideInset), std::min<int>(desired, std::min(limit, available)));
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
}
int ReadingSheetHeaderHeight() {
    return std::min<lv_coord_t>(UiSy(56), LV_VER_RES / 8);
}
void BuildReadingSheetHeader(lv_obj_t* header, const char* header_title, lv_event_cb_t back_event, bool back_arrow) {
    const int header_h = ReadingSheetHeaderHeight();
    const int title_x = UiSx(56);
    auto* title = SheetLabel(header, header_title, Book_ListFont());
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, title_x, 0);
    auto* bold_title = SheetLabel(header, header_title, Book_ListFont());
    lv_obj_set_style_text_align(bold_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(bold_title, LV_ALIGN_LEFT_MID, title_x + 1, 0);
    auto* close = SheetButton(header, "", UiSx(44), header_h - 8, back_event);
    lv_obj_set_style_border_width(close, 0, 0);lv_obj_align(close, LV_ALIGN_LEFT_MID, UiSx(8), 0);
    // Draw a fixed-size chevron: its size does not depend on the default icon font.
    static const lv_point_precise_t down_points[] = {
        {0, 0}, {12, 10}, {24, 0}
    };
    auto* down = lv_line_create(close);
    static const lv_point_precise_t back_points[] = {{12, 0}, {0, 10}, {12, 20}};
    lv_line_set_points(down, back_arrow ? back_points : down_points, 3);
    lv_obj_set_style_line_width(down, 4, 0);
    lv_obj_set_style_line_color(down, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(down, true, 0);
    lv_obj_clear_flag(down, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(down);
}
lv_obj_t* CreateReadingResetButton(lv_obj_t* header, lv_event_cb_t event) {
    const int height = ReadingSheetHeaderHeight();
    auto* reset = SheetButton(header, Lang::Strings::BOOK_TAP_ZONES_RESET, UiSx(100), height - UiSy(16), event);
    lv_obj_set_style_radius(reset, LV_RADIUS_CIRCLE, 0);
    ScreenFitLabel(lv_obj_get_child(reset, 0), UiSx(90), height - UiSy(20), SheetUiFont(20));
    lv_obj_center(lv_obj_get_child(reset, 0));
    lv_obj_align(reset, LV_ALIGN_RIGHT_MID, -UiSx(12), 0);
    return reset;
}
void SelectReadingSheetTab(int tab) {
    if (tab < 0 || tab > 3) return;
    const auto& settings = Book_State().settings;
    if (Book_State().tap.tap_zone_ui == BookUiState::TapZoneUi::kClosed && tab != 1 && !(tab == 3 && settings.reader_settings_page != kSettingsHome) && tab == settings.reader_tab && s_live_tab == tab && s_live_body &&
        s_live_body == settings.reader_sheet_body && lv_obj_is_valid(s_live_body) &&
        s_live_width == LV_HOR_RES && s_live_height == LV_VER_RES) return;
    if(tab!=2)ResetBookmarkSelection();
    TapZonesPanelClose();TtfPanelClose();
    Book_State().settings.reader_tab = tab;
    if (tab == 3) Book_State().settings.reader_settings_page = kSettingsHome;
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
    ApplyReadingPanelRules(sheet, ReadingSheetNavReserve() - 2);
    Book_DisableScroll(sheet);lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ResizeReadingBottomSheet();
    lv_obj_update_layout(sheet);
    const int sheet_width = lv_obj_get_content_width(sheet);
    const bool duration = st.settings.reader_tab == kDurationTab;
    const bool landscape_bookmarks=st.settings.reader_tab==2 && LV_HOR_RES>LV_VER_RES;
    const int header_h = duration ? 0 : landscape_bookmarks
        ? std::min<int>(UiSy(56),Book_ListFont()->line_height+12)
        : ReadingSheetHeaderHeight();
    const int nav_h = SheetNavHeight()+4; // Keep icons at their existing height; add 4px above them.
    if (!duration) {
        auto* header = SheetBox(sheet, sheet_width, header_h);
        const char* titles[] = {Lang::Strings::BOOK_TOC, Lang::Strings::BOOK_FONT_SELECT, Lang::Strings::BOOK_BOOKMARKS, Lang::Strings::BOOK_READING_SETTINGS};
        char font_selection[32];
        const char* header_title = st.settings.reader_tab == 3 ? SettingsPageTitle(st.settings.reader_settings_page) : titles[st.settings.reader_tab];
        if (st.settings.reader_tab == 1 && st.settings.font_multi) {
            std::snprintf(font_selection, sizeof(font_selection), Lang::Strings::BOOK_SELECTED_FMT, FontSelectedCount());
            header_title = font_selection;
        }
        BuildReadingSheetHeader(header, header_title, SettingsBack, st.settings.reader_tab == 3 && st.settings.reader_settings_page != kSettingsHome);
        if ((st.settings.reader_tab == 1 && !st.settings.font_multi) ||
            (st.settings.reader_tab == 3 && st.settings.reader_settings_page == kSettingsHome)) {
            CreateReadingResetButton(header, SettingsReset);
        }
        if(st.settings.reader_tab==2 && st.settings.bookmark_multi) {
            char count[32];std::snprintf(count,sizeof(count),Lang::Strings::BOOK_SELECTED_ITEMS_FMT,static_cast<unsigned>(st.settings.bookmark_selected.size()));
            auto* label=SheetLabel(header,count,Book_ListFont());
            lv_obj_set_style_bg_color(label,lv_color_white(),0);lv_obj_set_style_bg_opa(label,LV_OPA_COVER,0);lv_obj_center(label);
        }
    }
    lv_obj_update_layout(sheet);
    auto* body = SheetBox(sheet, sheet_width, lv_obj_get_height(sheet) - header_h - nav_h);
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
    auto* nav = SheetBox(sheet, sheet_width, nav_h);lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
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
            auto* button=SheetButton(nav,actions[i],sheet_width/3-UiSx(16),std::min<int>(UiSy(34),nav_h-bottom_gap-6),BookmarkActionEvent,codes[i]);
            lv_obj_set_pos(button,sheet_width*i/3+UiSx(8),UiSy(16));
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
            auto* button = SheetButton(nav, actions[i], sheet_width / 3 - UiSx(16),
                std::min<int>(UiSy(34), nav_h - bottom_gap - 6), callbacks[i]);
            lv_obj_set_pos(button, sheet_width * i / 3 + UiSx(8), UiSy(16));
            if (i == 2) SetSheetBtnEnabled(button,
                (ttf_multi ? TtfSelectedCount() : FontSelectedCount()) > 0 && (ttf_multi || !ReaderWorkersBusy()));
        }
    } else for (int i = 0; i < 4; ++i) {
        auto* btn = SheetBox(nav, sheet_width / 4, nav_h - bottom_gap - 2);lv_obj_set_pos(btn, i * sheet_width / 4, 1);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, SheetTabEvent, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        auto* icon = lv_image_create(btn);
        lv_image_set_src(icon, icons[i]);
        if(i==2)lv_obj_set_style_image_recolor(icon,lv_color_black(),0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, LV_HOR_RES>LV_VER_RES?8:UiSy(5)+4);
        auto* label = SheetLabel(btn, tabs[i], ReadingCardStyle() ? SheetUiFont(20, st.settings.reader_tab == i, tabs[i]) : st.settings.reader_tab == i ? Book_ListFont() : Book_ItemFont());lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, LV_HOR_RES>LV_VER_RES?-6:-UiSy(8));
        if (st.settings.reader_tab == i) {
            // The bundled UI font has no bold face: a one-pixel overstrike thickens its strokes.
            if (!ReadingCardStyle()) {
                auto* bold = SheetLabel(btn, tabs[i], Book_ListFont());
                lv_obj_align(bold, LV_ALIGN_BOTTOM_MID, 1, LV_HOR_RES>LV_VER_RES?-6:-UiSy(8));
            }
            lv_obj_update_layout(btn);
            auto* mark = SheetBox(btn, ReadingCardStyle() ? lv_obj_get_width(label) : UiSx(44), UiSy(2));
            lv_obj_set_style_bg_color(mark, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
            lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, 0);
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
        if(st.settings.reader_tab==0 || st.settings.reader_tab==1)
            lv_obj_send_event(st.settings.reader_sheet_scroll,LV_EVENT_SCROLL,nullptr);
    }
    s_live_body=body;s_live_tab=st.settings.reader_tab;
    s_live_width=LV_HOR_RES;s_live_height=LV_VER_RES;
    s_live_settings_page=st.settings.reader_settings_page;
    ApplyReadingPanelRules(sheet,nav_h-2);
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) TtfEnsurePanel();
    if (st.tap.tap_zone_ui != BookUiState::TapZoneUi::kClosed) TapZonesEnsurePanel();
    if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings) SetReadStatusVisible(true);
}


