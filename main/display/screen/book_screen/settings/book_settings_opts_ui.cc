#pragma GCC optimize("O1")

#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/book_text_util.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <cstdio>
#include <lvgl.h>

#include "assets/lang_config.h"
#include "frontlight.h"
#include "haptic_feedback.h"

void SettingsSheetPopulateOptions(BookUiState& st, lv_obj_t* parent) {
auto add_title_with_bound = [&](lv_obj_t* parent, const char* title, bool can_dec,
                                bool can_inc, bool center, lv_obj_t** out_bound) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);

    lv_obj_t* t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, Book_ItemFont(), 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

    // 始终建 tip，原地 Refresh 显隐 MIN/MAX
    lv_obj_t* tip = lv_label_create(row);
    lv_obj_set_style_text_font(tip, Book_ItemFont(), 0);
    lv_obj_set_style_text_opa(tip, LV_OPA_70, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_CLICKABLE);
    UpdateSheetBoundTip(tip, can_dec, can_inc);
    if (out_bound != nullptr) {
        *out_bound = tip;
    }
};

lv_obj_t* pair = lv_obj_create(parent);
lv_obj_remove_style_all(pair);
lv_obj_set_width(pair, lv_pct(100));
lv_obj_set_height(pair, LV_SIZE_CONTENT);
lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
lv_obj_set_style_pad_column(pair, kSheetGap, 0);
lv_obj_clear_flag(pair, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(pair);

const int margin_i = BookReaderPrefsMarginPreset();
const int gap_i = BookReaderPrefsSpacingPreset();

auto make_adj = [&](const char* title, const char* value, lv_event_cb_t dec, lv_event_cb_t inc,
                    bool can_dec, bool can_inc, lv_obj_t** out_value, lv_obj_t** out_dec,
                    lv_obj_t** out_inc, lv_obj_t** out_bound) {
    lv_obj_t* card = lv_obj_create(pair);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, kSheetBorderW, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(card);

    add_title_with_bound(card, title, can_dec, can_inc, true, out_bound);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSheetIconBtn);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);
    lv_obj_t* dec_btn = MakeSheetIconBtn(row, "-", dec, can_dec);
    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, Book_ListFont(), 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* inc_btn = MakeSheetIconBtn(row, "+", inc, can_inc);
    if (out_value != nullptr) {
        *out_value = v;
    }
    if (out_dec != nullptr) {
        *out_dec = dec_btn;
    }
    if (out_inc != nullptr) {
        *out_inc = inc_btn;
    }
};
make_adj(Lang::Strings::BOOK_MARGIN, BookReaderPrefsMarginLabel(margin_i), OnSheetMarginDec, OnSheetMarginInc,
         margin_i > 0, margin_i + 1 < kBookReaderMarginPresetCount, &st.settings.sheet_margin_value,
         &st.settings.sheet_margin_dec, &st.settings.sheet_margin_inc, &st.settings.sheet_margin_bound);
make_adj(Lang::Strings::BOOK_LINE_GAP, BookReaderPrefsSpacingLabel(gap_i), OnSheetGapDec, OnSheetGapInc, gap_i > 0,
         gap_i + 1 < kBookReaderSpacingPresetCount, &st.settings.sheet_gap_value, &st.settings.sheet_gap_dec,
         &st.settings.sheet_gap_inc, &st.settings.sheet_gap_bound);

// 边框画在对象内侧：高度/内边距按「描边+垫+钮」对齐，避免选中黑块相对圆角偏移
const lv_coord_t seg_h = kSegBtnH + kSegPad * 2 + kSheetBorderW * 2;
// 底栏字段多选（左标题右椭圆）：全关则不占位；默认章节名+进度
{
    const int mask = BookReaderPrefsFooterMask();
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsOptsRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);
    lv_obj_t* lab = lv_label_create(row);
    lv_label_set_text(lab, Lang::Strings::BOOK_CHAPTER_PROGRESS);
    lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* seg = lv_obj_create(row);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
    lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
    lv_obj_set_style_border_color(seg, lv_color_black(), 0);
    lv_obj_set_style_radius(seg, seg_h / 2, 0);
    lv_obj_set_style_pad_all(seg, kSegPad, 0);
    lv_obj_set_style_pad_column(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(seg);

    auto make_bit = [&](const char* txt, int bit) {
        const bool on = (mask & bit) != 0;
        lv_obj_t* b = lv_obj_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, kSegBtnH);
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
        lv_obj_set_style_bg_color(b, on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(b);
        lv_obj_add_event_cb(b, OnSheetFooterBitToggle, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(bit)));
        lv_obj_add_event_cb(
            b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(l, on ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(l);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    // 与底栏显示序一致：章节名 → 时间 → 进度 → 电量
    make_bit(Lang::Strings::BOOK_FOOTER_TITLE, kBookReaderFooterTitle);
    make_bit(Lang::Strings::BOOK_FOOTER_TIME, kBookReaderFooterTime);
    make_bit(Lang::Strings::BOOK_FOOTER_PROGRESS, kBookReaderFooterProgress);
    make_bit(Lang::Strings::BOOK_FOOTER_BATTERY, kBookReaderFooterBattery);
}
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsOptsRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);
    lv_obj_t* lab = lv_label_create(row);
    lv_label_set_text(lab, Lang::Strings::BOOK_ORIENT);
    lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* seg = lv_obj_create(row);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
    lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
    lv_obj_set_style_border_color(seg, lv_color_black(), 0);
    lv_obj_set_style_radius(seg, seg_h / 2, 0);
    lv_obj_set_style_pad_all(seg, kSegPad, 0);
    lv_obj_set_style_pad_column(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(seg);
    const int ori = BookReaderPrefsOrient();
    auto make_ori = [&](const char* txt, int mode) {
        const bool selected = (ori == mode);
        lv_obj_t* b = lv_obj_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, kOriBtnW, kSegBtnH);
        lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
        lv_obj_set_style_bg_color(b, selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(b);
        lv_obj_add_event_cb(b, OnSheetOrientMode, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
        lv_obj_add_event_cb(
            b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(l);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    make_ori(Lang::Strings::BOOK_ORIENT_PORTRAIT, kBookReaderOrientPortrait);
    make_ori(Lang::Strings::BOOK_ORIENT_LAND_LEFT, kBookReaderOrientLandLeft);
    make_ori(Lang::Strings::BOOK_ORIENT_LAND_RIGHT, kBookReaderOrientLandRight);
}

// 下划线：实线 / 虚线 / 关
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsOptsRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);
    lv_obj_t* lab = lv_label_create(row);
    lv_label_set_text(lab, Lang::Strings::BOOK_UNDERLINE);
    lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* seg = lv_obj_create(row);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
    lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
    lv_obj_set_style_border_color(seg, lv_color_black(), 0);
    lv_obj_set_style_radius(seg, seg_h / 2, 0);
    lv_obj_set_style_pad_all(seg, kSegPad, 0);
    lv_obj_set_style_pad_column(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(seg);
    const int ul = BookReaderPrefsUnderlineMode();
    auto make_ul = [&](const char* txt, int mode) {
        const bool selected = (ul == mode);
        lv_obj_t* b = lv_obj_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, kUlBtnW, kSegBtnH);
        lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
        lv_obj_set_style_bg_color(b, selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(b);
        lv_obj_add_event_cb(b, OnSheetUnderlineMode, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
        lv_obj_add_event_cb(
            b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(l);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    make_ul(Lang::Strings::BOOK_UNDERLINE_SOLID, kBookReaderUnderlineSolid);
    make_ul(Lang::Strings::BOOK_UNDERLINE_DASHED, kBookReaderUnderlineDashed);
    make_ul(Lang::Strings::COMMON_OFF, kBookReaderUnderlineOff);
}

// 前光色温：冷 / 暖 / 冷暖 / 关（与设置→前光同套 NVS）
{
    auto& fl = Frontlight::GetInstance();
    const FrontlightCct cur =
        fl.initialized() ? fl.cct() : FrontlightCct::kOff;
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsOptsRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);
    lv_obj_t* lab = lv_label_create(row);
    lv_label_set_text(lab, Lang::Strings::SETTINGS_FRONTLIGHT_TITLE);
    lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* seg = lv_obj_create(row);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
    lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
    lv_obj_set_style_border_color(seg, lv_color_black(), 0);
    lv_obj_set_style_radius(seg, seg_h / 2, 0);
    lv_obj_set_style_pad_all(seg, kSegPad, 0);
    lv_obj_set_style_pad_column(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(seg);
    auto make_fl = [&](const char* txt, FrontlightCct mode) {
        const bool selected = (cur == mode);
        lv_obj_t* b = lv_obj_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, kSegBtnH);
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 6, 0);
        lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
        lv_obj_set_style_bg_color(b, selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(b);
        lv_obj_add_event_cb(b, OnSheetFlCct, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int>(mode))));
        lv_obj_add_event_cb(
            b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(l);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    make_fl(Lang::Strings::SETTINGS_FRONTLIGHT_COOL, FrontlightCct::kCool);
    make_fl(Lang::Strings::SETTINGS_FRONTLIGHT_WARM, FrontlightCct::kWarm);
    make_fl(Lang::Strings::SETTINGS_FRONTLIGHT_BOTH, FrontlightCct::kBoth);
    make_fl(Lang::Strings::SETTINGS_FRONTLIGHT_OFF, FrontlightCct::kOff);
}

// 前光亮度 ±（步进 5，落盘同设置页）
{
    auto& fl = Frontlight::GetInstance();
    const int bri = fl.initialized() ? static_cast<int>(fl.target_brightness()) : 0;
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsOptsRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(row);

    MakeSheetIconBtn(row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC, OnSheetFlBrightDec,
                     true);
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT, bri);
    st.settings.sheet_fl_bright_value = lv_label_create(row);
    lv_label_set_text(st.settings.sheet_fl_bright_value, buf);
    lv_obj_set_style_text_font(st.settings.sheet_fl_bright_value, Book_ItemFont(), 0);
    lv_obj_clear_flag(st.settings.sheet_fl_bright_value, LV_OBJ_FLAG_CLICKABLE);
    MakeSheetIconBtn(row, Lang::Strings::SETTINGS_FRONTLIGHT_BRIGHTNESS_INC, OnSheetFlBrightInc,
                     true);
}

lv_obj_t* bar = lv_obj_create(parent);
lv_obj_remove_style_all(bar);
lv_obj_set_width(bar, lv_pct(100));
lv_obj_set_height(bar, 40);
lv_obj_set_style_pad_top(bar, 6, 0);
lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
lv_obj_set_style_border_width(bar, kSheetBorderW, 0);
lv_obj_set_style_border_color(bar, lv_color_black(), 0);
lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
lv_obj_set_style_pad_column(bar, 16, 0);
lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
Book_DisableScroll(bar);

MakeSheetTextLink(bar, Lang::Strings::BOOK_TOC, OnSheetTocClicked);
MakeSheetTextLink(bar, Lang::Strings::BOOK_TAP_ZONES, OnSheetTapZonesClicked);

lv_obj_update_layout(st.settings.settings_sheet);
// 浮层互斥：分区配置优先于 TTF 导入面板
if (st.tap.tap_zone_ui != BookUiState::TapZoneUi::kClosed) {
    TapZonesEnsurePanel();
} else if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) {
    TtfEnsurePanel();
}
}

