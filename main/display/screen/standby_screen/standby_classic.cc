#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_ui.h"
#include "standby_classic_weather_store.h"
#include "standby_classic_weather_fetch.h"
#include "standby_classic_todos.h"
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <lvgl.h>

#include "assets/lang_config.h"
#include "haptic_feedback.h"

void StandbyClassic_RefreshDateOnce() {
    if (StandbyClassic_State().date_lbl == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < (2020 - 1900)) {
        lv_label_set_text(StandbyClassic_State().date_lbl, Lang::Strings::STANDBY_DATE_PLACEHOLDER);
        return;
    }
    char date_str[48];
    std::snprintf(date_str, sizeof(date_str), Lang::Strings::STANDBY_DATE_FMT, tm_info.tm_mon + 1,
                  tm_info.tm_mday);
    lv_label_set_text(StandbyClassic_State().date_lbl, date_str);
}

void StandbyClassic_ApplyWeatherUi() {
    if (!StandbyClassic_alive || StandbyClassic_State().weather_row == nullptr) {
        return;
    }
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    if (!snap.valid) {
        lv_obj_add_flag(StandbyClassic_State().weather_row, LV_OBJ_FLAG_HIDDEN);
        if (StandbyClassic_State().meta_split != nullptr) {
            lv_obj_add_flag(StandbyClassic_State().meta_split, LV_OBJ_FLAG_HIDDEN);
        }
        if (StandbyClassic_State().lunar_lbl != nullptr) {
            lv_label_set_text(StandbyClassic_State().lunar_lbl, "");
        }
        return;
    }

    if (StandbyClassic_State().lunar_lbl != nullptr) {
        char sub[24];
        StandbyClassic_FormatLunarOrWeekday(sub, sizeof(sub), snap.lunar);
        lv_label_set_text(StandbyClassic_State().lunar_lbl, sub);
    }
    if (StandbyClassic_State().weather_text != nullptr) {
        lv_label_set_text(StandbyClassic_State().weather_text, StandbyClassic_LocalizedPhenomText(snap));
    }
    if (StandbyClassic_State().weather_temp != nullptr) {
        if (snap.has_temp) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d℃", snap.temp);
            lv_label_set_text(StandbyClassic_State().weather_temp, buf);
        } else {
            lv_label_set_text(StandbyClassic_State().weather_temp, "");
        }
    }
    if (StandbyClassic_State().weather_icon != nullptr) {
        if (snap.icon_code[0] != '\0') {
            char path[48];
            std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", snap.icon_code);
            lv_image_set_src(StandbyClassic_State().weather_icon, path);
            lv_obj_remove_flag(StandbyClassic_State().weather_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(StandbyClassic_State().weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(StandbyClassic_State().weather_row, LV_OBJ_FLAG_HIDDEN);
    if (StandbyClassic_State().meta_split != nullptr) {
        lv_obj_remove_flag(StandbyClassic_State().meta_split, LV_OBJ_FLAG_HIDDEN);
    }
}

void StandbyClassic_BuildUi(lv_obj_t* root) {
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    StandbyClassic_todos.clear();

    const lv_coord_t content_w = LV_HOR_RES - 2 * kSidePad;
    const lv_coord_t content_h = LV_VER_RES - kTopPad;

    lv_obj_t* center = lv_obj_create(root);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, content_w, content_h);
    lv_obj_align(center, LV_ALIGN_TOP_MID, 0, kTopPad);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center, 0, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spacer = lv_obj_create(center);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, lv_pct(100));
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* meta_row = lv_obj_create(center);
    lv_obj_remove_style_all(meta_row);
    lv_obj_set_width(meta_row, lv_pct(100));
    lv_obj_set_height(meta_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(meta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta_row, kMetaGap, 0);
    lv_obj_set_style_margin_bottom(meta_row, kSectionGap, 0);
    lv_obj_clear_flag(meta_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(meta_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* date_row = lv_obj_create(meta_row);
    lv_obj_remove_style_all(date_row);
    lv_obj_set_size(date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(date_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, kCalendarGap, 0);
    lv_obj_clear_flag(date_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(date_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cal_icon = lv_image_create(date_row);
    lv_image_set_src(cal_icon, "A:ic_s_standby_calendar.spng");
    lv_obj_set_size(cal_icon, kCalendarIconW, kCalendarIconH);
    lv_obj_clear_flag(cal_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* date_col = lv_obj_create(date_row);
    lv_obj_remove_style_all(date_col);
    lv_obj_set_size(date_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(date_col, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(date_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(date_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(date_col, 4, 0);
    lv_obj_clear_flag(date_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(date_col, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t* wfont = StandbyClassic_WeatherFont();
    StandbyClassic_State().date_lbl = lv_label_create(date_col);
    lv_label_set_text(StandbyClassic_State().date_lbl, Lang::Strings::STANDBY_DATE_PLACEHOLDER);
    lv_obj_set_style_text_font(StandbyClassic_State().date_lbl, wfont, 0);
    lv_obj_set_style_text_color(StandbyClassic_State().date_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(StandbyClassic_State().date_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(StandbyClassic_State().date_lbl, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().lunar_lbl = lv_label_create(date_col);
    lv_label_set_text(StandbyClassic_State().lunar_lbl, "");
    lv_obj_set_style_text_font(StandbyClassic_State().lunar_lbl, wfont, 0);
    lv_obj_set_style_text_color(StandbyClassic_State().lunar_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(StandbyClassic_State().lunar_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(StandbyClassic_State().lunar_lbl, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().meta_split = lv_obj_create(meta_row);
    lv_obj_remove_style_all(StandbyClassic_State().meta_split);
    lv_obj_set_size(StandbyClassic_State().meta_split, kMetaSplitW, kWeatherIcon);
    lv_obj_set_style_bg_color(StandbyClassic_State().meta_split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(StandbyClassic_State().meta_split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(StandbyClassic_State().meta_split, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(StandbyClassic_State().meta_split, LV_OBJ_FLAG_HIDDEN);

    StandbyClassic_State().weather_row = lv_obj_create(meta_row);
    lv_obj_remove_style_all(StandbyClassic_State().weather_row);
    lv_obj_set_size(StandbyClassic_State().weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(StandbyClassic_State().weather_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(StandbyClassic_State().weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(StandbyClassic_State().weather_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(StandbyClassic_State().weather_row, kWeatherGap, 0);
    lv_obj_clear_flag(StandbyClassic_State().weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(StandbyClassic_State().weather_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(StandbyClassic_State().weather_row, LV_OBJ_FLAG_HIDDEN);

    StandbyClassic_State().weather_icon = lv_image_create(StandbyClassic_State().weather_row);
    lv_obj_set_size(StandbyClassic_State().weather_icon, kWeatherIcon, kWeatherIcon);
    lv_obj_clear_flag(StandbyClassic_State().weather_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* meta = lv_obj_create(StandbyClassic_State().weather_row);
    lv_obj_remove_style_all(meta);
    lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(meta, 4, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().weather_text = lv_label_create(meta);
    lv_label_set_text(StandbyClassic_State().weather_text, "");
    lv_obj_set_style_text_font(StandbyClassic_State().weather_text, wfont, 0);
    lv_obj_set_style_text_color(StandbyClassic_State().weather_text, lv_color_black(), 0);
    lv_obj_clear_flag(StandbyClassic_State().weather_text, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().weather_temp = lv_label_create(meta);
    lv_label_set_text(StandbyClassic_State().weather_temp, "");
    lv_obj_set_style_text_font(StandbyClassic_State().weather_temp, wfont, 0);
    lv_obj_set_style_text_color(StandbyClassic_State().weather_temp, lv_color_black(), 0);
    lv_obj_clear_flag(StandbyClassic_State().weather_temp, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().todo_host = lv_obj_create(center);
    lv_obj_remove_style_all(StandbyClassic_State().todo_host);
    lv_obj_set_width(StandbyClassic_State().todo_host, lv_pct(100));
    lv_obj_set_height(StandbyClassic_State().todo_host, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(StandbyClassic_State().todo_host, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(StandbyClassic_State().todo_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(StandbyClassic_State().todo_host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(StandbyClassic_State().todo_host, kTodoGap, 0);
    lv_obj_set_style_margin_bottom(StandbyClassic_State().todo_host, kSectionGap, 0);
    lv_obj_clear_flag(StandbyClassic_State().todo_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(StandbyClassic_State().todo_host, LV_OBJ_FLAG_CLICKABLE);

    StandbyClassic_State().home_btn = lv_obj_create(center);
    lv_obj_remove_style_all(StandbyClassic_State().home_btn);
    lv_obj_set_size(StandbyClassic_State().home_btn, content_w, kHomeBtnH);
    lv_obj_set_style_bg_color(StandbyClassic_State().home_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(StandbyClassic_State().home_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(StandbyClassic_State().home_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(StandbyClassic_State().home_btn, kBorderW, 0);
    lv_obj_set_style_radius(StandbyClassic_State().home_btn, 8, 0);
    lv_obj_set_style_margin_bottom(StandbyClassic_State().home_btn, kHeroSectionGap, 0);
    lv_obj_clear_flag(StandbyClassic_State().home_btn, LV_OBJ_FLAG_SCROLLABLE);
    if (!StandbyClassic_as_overlay) {
        lv_obj_add_flag(StandbyClassic_State().home_btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(StandbyClassic_State().home_btn);
        lv_obj_add_event_cb(StandbyClassic_State().home_btn, StandbyClassic_OnHomeBtnClicked, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_flag(StandbyClassic_State().home_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(StandbyClassic_State().home_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_margin_bottom(StandbyClassic_State().home_btn, 0, 0);
    }

    lv_obj_t* menu_icon = lv_image_create(StandbyClassic_State().home_btn);
    lv_image_set_src(menu_icon, "A:ic_s_standby_menu.spng");
    lv_obj_set_size(menu_icon, kMenuIcon, kMenuIcon);
    lv_obj_center(menu_icon);
    lv_obj_clear_flag(menu_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hero = lv_obj_create(center);
    lv_obj_remove_style_all(hero);
    lv_obj_set_width(hero, content_w);
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hero, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hero, kHeroInnerGap, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* bubble = lv_image_create(hero);
    lv_image_set_src(bubble, "A:ic_s_standby_bubble.spng");
    lv_obj_set_size(bubble, kBubbleW, kBubbleH);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cat = lv_image_create(hero);
    lv_image_set_src(cat, "A:ic_s_standby_cat.spng");
    lv_obj_set_size(cat, kCatW, kCatH);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_CLICKABLE);
}

void StandbyClassic_StartOverlayRuntime() {
    StandbyClassic_RefreshDateOnce();
    StandbyClassic_ApplyWeatherUi();
    StandbyClassic_ApplyTodosFromCache();
    StandbyClassic_ScheduleWeatherEnsure();
}

void StandbyClassic_Build(lv_obj_t* root, bool as_overlay) {
    if (root == nullptr) {
        return;
    }
    StandbyClassic_as_overlay = as_overlay;
    StandbyClassic_alive = true;
    StandbyClassic_State() = StandbyClassicUiState{};
    StandbyClassic_BuildUi(root);
}

void StandbyClassic_StartRuntime() {
    if (!StandbyClassic_alive) {
        return;
    }
    StandbyClassic_StartOverlayRuntime();
}

void StandbyClassic_StopRuntime() {}

void StandbyClassic_Teardown() {
    StandbyClassic_StopRuntime();
    StandbyClassic_alive = false;
    StandbyClassic_as_overlay = false;
    StandbyClassic_todos.clear();
    StandbyClassic_State() = StandbyClassicUiState{};
}
