#pragma GCC optimize("O1")

#include "book_screen/settings/book_settings_sheet.h"
#include "book_screen/book_screen_priv.h"
#include "display_orient.h"
#include "lv_adapter_display.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <cstdio>
#include <lvgl.h>
#include <dirent.h>

#include "assets/lang_config.h"
#include "frontlight.h"
#include "haptic_feedback.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* kFlNvsNs = "frontlight";
constexpr const char* kFlNvsKeyCct = "cct";
constexpr int kFlBrightnessStep = 5;
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
    ScheduleLayoutApply(false);
}

void OnSheetFooterBitToggle(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int bit = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const int before = BookReaderPrefsFooterMask();
    const int after = BookReaderPrefsToggleFooterBit(bit);
    RebuildSettingsSheet();
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
    if (mode < 0 || mode >= kBookReaderOrientCount) {
        return;
    }
    if (BookReaderPrefsOrient() == mode) {
        return;
    }
    BookReaderPrefsSetOrient(mode);
    DisplayUiSetOrient(mode);
    ApplyReaderChromeSize();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    RebuildSettingsSheet();
    Book_State().reader.settings_resume_after_open = true;
    ReopenReaderAfterPrefsChange();
}

void OnSheetUnderlineMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        return;
    }
    if (BookReaderPrefsUnderlineMode() == mode) {
        return;
    }
    BookReaderPrefsSetUnderlineMode(mode);
    RebuildSettingsSheet();
    RenderReaderPage();
}

void OnSheetFlCct(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto cct = NormalizeFlCct(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    auto& fl = Frontlight::GetInstance();
    if (!fl.initialized()) {
        ESP_LOGW(kFlTag, "frontlight not ready");
        return;
    }
    if (fl.cct() == cct) {
        return;
    }
    fl.SetCct(cct);
    const int value = static_cast<int>(cct);
    if (xTaskCreate(PersistFlCctTask, "book_fl_cct", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(value)), 5, nullptr) != pdPASS) {
        ESP_LOGE(kFlTag, "xTaskCreate(book_fl_cct) failed");
    }
    RebuildSettingsSheet();
}

void OnSheetFlBrightDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    ApplySheetFlBrightnessDelta(-kFlBrightnessStep);
}

void OnSheetFlBrightInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    ApplySheetFlBrightnessDelta(kFlBrightnessStep);
}

void RebuildSettingsSheet() {
    auto& st = Book_State();
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    ClearSettingsSheetWidgetRefs();
    lv_obj_clean(st.settings.settings_sheet);
    lv_obj_set_style_min_height(st.settings.settings_sheet, 0, 0);
    // 字库列表只扫一次；点选/翻页勿每次 SD readdir
    if (st.settings.font_entries.empty()) {
        ScanReadFonts();
    }
    SyncFontSelectedSize();
    ClampFontListPage();
    lv_obj_t* font_host = st.settings.settings_sheet;
    lv_obj_t* opts_host = st.settings.settings_sheet;
    lv_obj_t* cols = nullptr;
    lv_obj_t* rule = nullptr;
    if (DisplayUiIsLandscape()) {
        cols = lv_obj_create(st.settings.settings_sheet);
        lv_obj_remove_style_all(cols);
        lv_obj_set_width(cols, lv_pct(100));
        lv_obj_set_height(cols, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(cols, kSheetGap, 0);
        lv_obj_clear_flag(cols, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(cols);
        auto make_col = [&]() {
            lv_obj_t* col = lv_obj_create(cols);
            lv_obj_remove_style_all(col);
            lv_obj_set_height(col, LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(col, 1);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(col, kSheetGap, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(col);
            return col;
        };
        font_host = make_col();
        rule = Book_AddColRule(cols);
        opts_host = make_col();
    }
    SettingsSheetPopulateFont(st, font_host);
    SettingsSheetPopulateOptions(st, opts_host);
    Book_FinishColRule(cols, rule);
}

