#include "home_screen/home_hero_priv.h"

#include "home_hero_build.h"
#include <lvgl.h>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"

void HomeHero_OnSlashGapDraw(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_END) {
        return;
    }
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    // 斜线居中于时宽，长度适中、斜率缓于竖线
    const lv_coord_t mid_x = (a.x1 + a.x2) / 2;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_black();
    dsc.opa = LV_OPA_COVER;
    dsc.width = 2;
    dsc.round_start = 0;
    dsc.round_end = 0;
    dsc.p1.x = mid_x - kSlashLineHalfW;
    dsc.p1.y = a.y2 - kSlashLinePadY;
    dsc.p2.x = mid_x + kSlashLineHalfW;
    dsc.p2.y = a.y1 + kSlashLinePadY;
    lv_draw_line(layer, &dsc);
}

void HomeHero_StyleSlashTimeLabel(lv_obj_t* lbl) {
    lv_obj_set_style_text_font(lbl, HomeHero_SlashTimeFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(lbl, kSlashTimeW);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP); // 防宽度不足时第二位换行
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void HomeHero_BuildClassicUi(lv_obj_t* parent, lv_coord_t y_offset) {
    lv_obj_t* box = lv_obj_create(parent);
    HomeHero_State().root = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_HOR_RES - 2 * kSidePad);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, y_offset + kHeroPadTop);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(box, kTimeDateGap, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().time_lbl = lv_label_create(box);
    lv_label_set_text(HomeHero_State().time_lbl, "00:00");
    lv_obj_set_style_text_font(HomeHero_State().time_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(HomeHero_State().time_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(HomeHero_State().time_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 居中成组：日期 | 竖线 | 天气，用 pad_column 控与竖线边距（勿 SPACE_BETWEEN 拉满宽）
    lv_obj_t* meta_row = lv_obj_create(box);
    lv_obj_remove_style_all(meta_row);
    lv_obj_set_width(meta_row, LV_SIZE_CONTENT);
    lv_obj_set_height(meta_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(meta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta_row, kMetaGap, 0);
    lv_obj_clear_flag(meta_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(meta_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* date_row = lv_obj_create(meta_row);
    lv_obj_remove_style_all(date_row);
    lv_obj_set_size(date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(date_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, kCalendarGap, 0);
    lv_obj_clear_flag(date_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(date_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cal_icon = lv_image_create(date_row);
    lv_image_set_src(cal_icon, "A:ic_s_standby_calendar.spng");
    lv_obj_set_size(cal_icon, kCalendarIconW, kCalendarIconH);
    lv_image_set_inner_align(cal_icon, LV_IMAGE_ALIGN_CONTAIN);
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

    const lv_font_t* wfont = HomeHero_WeatherFont();
    HomeHero_State().date_lbl = lv_label_create(date_col);
    lv_label_set_text(HomeHero_State().date_lbl, Lang::Strings::HOME_DATE_PLACEHOLDER);
    lv_obj_set_style_text_font(HomeHero_State().date_lbl, wfont, 0);
    lv_obj_set_style_text_color(HomeHero_State().date_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().date_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(HomeHero_State().date_lbl, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().lunar_lbl = lv_label_create(date_col);
    lv_label_set_text(HomeHero_State().lunar_lbl, "");
    lv_obj_set_style_text_font(HomeHero_State().lunar_lbl, wfont, 0);
    lv_obj_set_style_text_color(HomeHero_State().lunar_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().lunar_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(HomeHero_State().lunar_lbl, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().meta_split = lv_obj_create(meta_row);
    lv_obj_remove_style_all(HomeHero_State().meta_split);
    lv_obj_set_size(HomeHero_State().meta_split, kMetaSplitW, kWeatherIcon);
    lv_obj_set_style_bg_color(HomeHero_State().meta_split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(HomeHero_State().meta_split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(HomeHero_State().meta_split, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(HomeHero_State().meta_split, LV_OBJ_FLAG_HIDDEN);

    HomeHero_State().weather_row = lv_obj_create(meta_row);
    lv_obj_remove_style_all(HomeHero_State().weather_row);
    lv_obj_set_size(HomeHero_State().weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(HomeHero_State().weather_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(HomeHero_State().weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(HomeHero_State().weather_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(HomeHero_State().weather_row, kWeatherGap, 0);
    lv_obj_clear_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);

    HomeHero_State().weather_icon = lv_image_create(HomeHero_State().weather_row);
    lv_obj_set_size(HomeHero_State().weather_icon, kWeatherIcon, kWeatherIcon);
    lv_image_set_inner_align(HomeHero_State().weather_icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_clear_flag(HomeHero_State().weather_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* meta = lv_obj_create(HomeHero_State().weather_row);
    lv_obj_remove_style_all(meta);
    lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(meta, 4, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().weather_text = lv_label_create(meta);
    lv_label_set_text(HomeHero_State().weather_text, "");
    lv_obj_set_style_text_font(HomeHero_State().weather_text, wfont, 0);
    lv_obj_set_style_text_color(HomeHero_State().weather_text, lv_color_black(), 0);
    lv_obj_clear_flag(HomeHero_State().weather_text, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().weather_temp = lv_label_create(meta);
    lv_label_set_text(HomeHero_State().weather_temp, "");
    lv_obj_set_style_text_font(HomeHero_State().weather_temp, wfont, 0);
    lv_obj_set_style_text_color(HomeHero_State().weather_temp, lv_color_black(), 0);
    lv_obj_clear_flag(HomeHero_State().weather_temp, LV_OBJ_FLAG_CLICKABLE);
}

void HomeHero_BuildSlashUi(lv_obj_t* parent, lv_coord_t y_offset) {
    lv_obj_t* box = lv_obj_create(parent);
    HomeHero_State().root = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_HOR_RES - 2 * kSlashSidePad);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, y_offset + kHeroPadTop);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    // 底对齐：日期块落在分针一带，星期一行才看得见
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(box, kSlashTimeDateGap, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* time_col = lv_obj_create(box);
    lv_obj_remove_style_all(time_col);
    lv_obj_set_size(time_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_col, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(time_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(time_col, 0, 0);
    lv_obj_clear_flag(time_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(time_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(time_col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    HomeHero_State().hour_lbl = lv_label_create(time_col);
    lv_label_set_text(HomeHero_State().hour_lbl, "00");
    HomeHero_StyleSlashTimeLabel(HomeHero_State().hour_lbl);

    lv_obj_t* slash_gap = lv_obj_create(time_col);
    lv_obj_remove_style_all(slash_gap);
    lv_obj_set_size(slash_gap, kSlashTimeW, kSlashSlashGapH);
    lv_obj_set_style_bg_opa(slash_gap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(slash_gap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slash_gap, HomeHero_OnSlashGapDraw, LV_EVENT_DRAW_MAIN_END, nullptr);

    HomeHero_State().minute_lbl = lv_label_create(time_col);
    lv_label_set_text(HomeHero_State().minute_lbl, "00");
    HomeHero_StyleSlashTimeLabel(HomeHero_State().minute_lbl);
    lv_obj_set_style_translate_x(HomeHero_State().minute_lbl, kSlashMinuteNudgeX, 0);

    lv_obj_t* date_col = lv_obj_create(box);
    lv_obj_remove_style_all(date_col);
    lv_obj_set_size(date_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(date_col, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(date_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(date_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(date_col, kSlashDateGap, 0);
    lv_obj_clear_flag(date_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(date_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(date_col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // 公历/星期常规；农历 30@4 半粗
    const lv_font_t* date_font = fontpack_lv_font_get(30, 2);
    if (date_font == nullptr) {
        date_font = fontpack_lv_font_ui();
    }
    const lv_font_t* lunar_font = fontpack_lv_font_get(30, 4);
    if (lunar_font == nullptr) {
        lunar_font = date_font;
    }

    // 天气在日期横线上方
    HomeHero_State().weather_row = lv_obj_create(date_col);
    lv_obj_remove_style_all(HomeHero_State().weather_row);
    lv_obj_set_size(HomeHero_State().weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(HomeHero_State().weather_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(HomeHero_State().weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(HomeHero_State().weather_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(HomeHero_State().weather_row, kSlashWeatherGap, 0);
    lv_obj_clear_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);

    HomeHero_State().weather_icon = lv_image_create(HomeHero_State().weather_row);
    lv_obj_set_size(HomeHero_State().weather_icon, kSlashWeatherIcon, kSlashWeatherIcon);
    // 资源 70×70：按控件等比完整装入，避免默认居中裁切导致缺边
    lv_image_set_inner_align(HomeHero_State().weather_icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_clear_flag(HomeHero_State().weather_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* weather_meta = lv_obj_create(HomeHero_State().weather_row);
    lv_obj_remove_style_all(weather_meta);
    lv_obj_set_size(weather_meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_meta, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(weather_meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather_meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(weather_meta, 2, 0);
    lv_obj_clear_flag(weather_meta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(weather_meta, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().weather_text = lv_label_create(weather_meta);
    lv_label_set_text(HomeHero_State().weather_text, "");
    // 天气描述半粗；温度保持常规
    const lv_font_t* weather_text_font = fontpack_lv_font_get(30, 4);
    if (weather_text_font == nullptr) {
        weather_text_font = date_font;
    }
    lv_obj_set_style_text_font(HomeHero_State().weather_text, weather_text_font, 0);
    lv_obj_set_style_text_color(HomeHero_State().weather_text, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().weather_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(HomeHero_State().weather_text, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().weather_temp = lv_label_create(weather_meta);
    lv_label_set_text(HomeHero_State().weather_temp, "");
    lv_obj_set_style_text_font(HomeHero_State().weather_temp, date_font, 0);
    lv_obj_set_style_text_color(HomeHero_State().weather_temp, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().weather_temp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(HomeHero_State().weather_temp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* rule = lv_obj_create(date_col);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, kSlashDateRuleW, kSlashDateRuleH);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().date_lbl = lv_label_create(date_col);
    lv_label_set_text(HomeHero_State().date_lbl, Lang::Strings::HOME_DATE_SLASH_PLACEHOLDER);
    lv_obj_set_style_text_font(HomeHero_State().date_lbl, date_font, 0);
    lv_obj_set_style_text_color(HomeHero_State().date_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().date_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(HomeHero_State().date_lbl, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().lunar_lbl = lv_label_create(date_col);
    lv_label_set_text(HomeHero_State().lunar_lbl, "");
    lv_obj_add_flag(HomeHero_State().lunar_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(HomeHero_State().lunar_lbl, lunar_font, 0);
    lv_obj_set_style_text_color(HomeHero_State().lunar_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().lunar_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(HomeHero_State().lunar_lbl, LV_OBJ_FLAG_CLICKABLE);

    HomeHero_State().weekday_lbl = lv_label_create(date_col);
    lv_label_set_text(HomeHero_State().weekday_lbl, "");
    lv_obj_set_style_text_font(HomeHero_State().weekday_lbl, date_font, 0);
    lv_obj_set_style_text_color(HomeHero_State().weekday_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(HomeHero_State().weekday_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(HomeHero_State().weekday_lbl, LV_OBJ_FLAG_CLICKABLE);
}

