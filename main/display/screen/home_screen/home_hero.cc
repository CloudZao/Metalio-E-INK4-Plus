#include "home_screen/home_hero_priv.h"

#include "home_hero_internal.h"
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <lvgl.h>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"

HomeHeroUiState& HomeHero_State() {
    static HomeHeroUiState s;
    return s;
}
bool HomeHero_alive = false;

const lv_font_t* HomeHero_SlashTimeFont() {
    static lv_font_t font;
    static bool inited = false;
    if (!inited) {
        font = font_misans_regular_160_2;
        font.line_height = 134;
        font.base_line = 10;
        inited = true;
    }
    return &font;
}

const lv_font_t* HomeHero_WeatherFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

const char* HomeHero_WeekdayName(int wday) {
    switch (wday) {
        case 0:
            return Lang::Strings::HOME_WDAY_SUN;
        case 1:
            return Lang::Strings::HOME_WDAY_MON;
        case 2:
            return Lang::Strings::HOME_WDAY_TUE;
        case 3:
            return Lang::Strings::HOME_WDAY_WED;
        case 4:
            return Lang::Strings::HOME_WDAY_THU;
        case 5:
            return Lang::Strings::HOME_WDAY_FRI;
        case 6:
            return Lang::Strings::HOME_WDAY_SAT;
        default:
            return "";
    }
}

void HomeHero_FormatClassicClock(char* time_buf, size_t time_len, char* date_buf, size_t date_len) {
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < (2020 - 1900)) {
        std::snprintf(time_buf, time_len, "00:00");
        std::snprintf(date_buf, date_len, Lang::Strings::HOME_DATE_PLACEHOLDER);
        return;
    }
    std::snprintf(time_buf, time_len, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    std::snprintf(date_buf, date_len, Lang::Strings::HOME_DATE_FMT, tm_info.tm_mon + 1,
                  tm_info.tm_mday);
}

void HomeHero_FormatSlashClock(char* hour_buf, size_t hour_len, char* min_buf, size_t min_len,
                      char* date_buf, size_t date_len, char* wday_buf, size_t wday_len) {
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < (2020 - 1900)) {
        std::snprintf(hour_buf, hour_len, "00");
        std::snprintf(min_buf, min_len, "00");
        std::snprintf(date_buf, date_len, Lang::Strings::HOME_DATE_SLASH_PLACEHOLDER);
        std::snprintf(wday_buf, wday_len, "%s", "");
        return;
    }
    std::snprintf(hour_buf, hour_len, "%02d", tm_info.tm_hour);
    std::snprintf(min_buf, min_len, "%02d", tm_info.tm_min);
    std::snprintf(date_buf, date_len, Lang::Strings::HOME_DATE_SLASH_FMT, tm_info.tm_year + 1900,
                  tm_info.tm_mon + 1, tm_info.tm_mday);
    std::snprintf(wday_buf, wday_len, "%s", HomeHero_WeekdayName(tm_info.tm_wday));
}

void HomeHero_RefreshClock(bool force) {
    if (!HomeHero_alive) {
        return;
    }

    if (HomeHero_State().kind == HeroKind::Slash) {
        if (HomeHero_State().hour_lbl == nullptr || HomeHero_State().minute_lbl == nullptr || HomeHero_State().date_lbl == nullptr) {
            return;
        }
        char hour[4];
        char minute[4];
        char date_str[48];
        char wday[24];
        HomeHero_FormatSlashClock(hour, sizeof(hour), minute, sizeof(minute), date_str, sizeof(date_str), wday,
                         sizeof(wday));
        char time_key[8];
        std::snprintf(time_key, sizeof(time_key), "%s%s", hour, minute);
        if (force || std::strcmp(time_key, HomeHero_State().last_time) != 0) {
            lv_label_set_text(HomeHero_State().hour_lbl, hour);
            lv_label_set_text(HomeHero_State().minute_lbl, minute);
            std::snprintf(HomeHero_State().last_time, sizeof(HomeHero_State().last_time), "%s", time_key);
        }
        if (force || std::strcmp(date_str, HomeHero_State().last_date) != 0) {
            lv_label_set_text(HomeHero_State().date_lbl, date_str);
            std::snprintf(HomeHero_State().last_date, sizeof(HomeHero_State().last_date), "%s", date_str);
        }
        if (HomeHero_State().weekday_lbl != nullptr && (force || std::strcmp(wday, HomeHero_State().last_weekday) != 0)) {
            lv_label_set_text(HomeHero_State().weekday_lbl, wday);
            std::snprintf(HomeHero_State().last_weekday, sizeof(HomeHero_State().last_weekday), "%s", wday);
        }
        return;
    }

    if (HomeHero_State().time_lbl == nullptr || HomeHero_State().date_lbl == nullptr) {
        return;
    }

    char time_str[8];
    char date_str[48];
    HomeHero_FormatClassicClock(time_str, sizeof(time_str), date_str, sizeof(date_str));

    if (force || std::strcmp(time_str, HomeHero_State().last_time) != 0) {
        lv_label_set_text(HomeHero_State().time_lbl, time_str);
        std::snprintf(HomeHero_State().last_time, sizeof(HomeHero_State().last_time), "%s", time_str);
    }
    if (force || std::strcmp(date_str, HomeHero_State().last_date) != 0) {
        lv_label_set_text(HomeHero_State().date_lbl, date_str);
        std::snprintf(HomeHero_State().last_date, sizeof(HomeHero_State().last_date), "%s", date_str);
    }
}

void HomeHero_FillWeatherWidgets(const StandbyClassicWeatherView& snap) {
    if (HomeHero_State().weather_text != nullptr) {
        lv_label_set_text(HomeHero_State().weather_text, snap.text);
    }
    if (HomeHero_State().weather_temp != nullptr) {
        if (snap.has_temp) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d℃", snap.temp);
            lv_label_set_text(HomeHero_State().weather_temp, buf);
        } else {
            lv_label_set_text(HomeHero_State().weather_temp, "");
        }
    }
    if (HomeHero_State().weather_icon != nullptr) {
        if (snap.icon_code[0] != '\0') {
            char path[48];
            std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", snap.icon_code);
            lv_image_set_src(HomeHero_State().weather_icon, path);
            lv_obj_remove_flag(HomeHero_State().weather_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(HomeHero_State().weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void HomeHero_ApplyWeatherUi() {
    if (!HomeHero_alive) {
        return;
    }

    StandbyClassicWeatherView snap;
    const bool weather_ok = StandbyClassic_CopyWeatherView(&snap) && snap.valid;

    if (HomeHero_State().kind == HeroKind::Slash) {
        if (HomeHero_State().lunar_lbl != nullptr) {
            char lunar[24] = {};
            if (weather_ok && std::strcmp(Lang::CODE, "zh-CN") == 0) {
                StandbyClassic_FormatLunarOrWeekday(lunar, sizeof(lunar), snap.lunar);
            }
            if (std::strcmp(lunar, HomeHero_State().last_lunar) != 0) {
                lv_label_set_text(HomeHero_State().lunar_lbl, lunar);
                std::snprintf(HomeHero_State().last_lunar, sizeof(HomeHero_State().last_lunar), "%s", lunar);
            }
            if (lunar[0] == '\0') {
                lv_obj_add_flag(HomeHero_State().lunar_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(HomeHero_State().lunar_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (HomeHero_State().weather_row == nullptr) {
            return;
        }
        if (!weather_ok) {
            lv_obj_add_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        HomeHero_FillWeatherWidgets(snap);
        lv_obj_remove_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (HomeHero_State().weather_row == nullptr) {
        return;
    }
    if (!weather_ok) {
        lv_obj_add_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);
        if (HomeHero_State().meta_split != nullptr) {
            lv_obj_add_flag(HomeHero_State().meta_split, LV_OBJ_FLAG_HIDDEN);
        }
        if (HomeHero_State().lunar_lbl != nullptr) {
            lv_label_set_text(HomeHero_State().lunar_lbl, "");
        }
        return;
    }

    if (HomeHero_State().lunar_lbl != nullptr) {
        char sub[24];
        StandbyClassic_FormatLunarOrWeekday(sub, sizeof(sub), snap.lunar);
        lv_label_set_text(HomeHero_State().lunar_lbl, sub);
    }
    HomeHero_FillWeatherWidgets(snap);
    lv_obj_remove_flag(HomeHero_State().weather_row, LV_OBJ_FLAG_HIDDEN);
    if (HomeHero_State().meta_split != nullptr) {
        lv_obj_remove_flag(HomeHero_State().meta_split, LV_OBJ_FLAG_HIDDEN);
    }
}

void HomeHero_OnWeatherListener() {
    HomeHero_ApplyWeatherUi();
}

void HomeHero_OnClockTimer(lv_timer_t* /*timer*/) {
    HomeHero_RefreshClock(false);
}

lv_obj_t* HomeHero_Mount(lv_obj_t* parent, lv_coord_t y_offset) {
    if (parent == nullptr) {
        return nullptr;
    }
    HomeHero_Teardown();
    HomeHero_State() = HomeHeroUiState{};
    HomeHero_State().kind = HeroKind::Classic;
    HomeHero_alive = true;
    HomeHero_BuildClassicUi(parent, y_offset);
    ESP_LOGI(TAG, "mounted classic y_offset=%d", static_cast<int>(y_offset));
    return HomeHero_State().root;
}

lv_obj_t* HomeHero_MountSlash(lv_obj_t* parent, lv_coord_t y_offset) {
    if (parent == nullptr) {
        return nullptr;
    }
    HomeHero_Teardown();
    HomeHero_State() = HomeHeroUiState{};
    HomeHero_State().kind = HeroKind::Slash;
    HomeHero_alive = true;
    HomeHero_BuildSlashUi(parent, y_offset);
    ESP_LOGI(TAG, "mounted slash y_offset=%d", static_cast<int>(y_offset));
    return HomeHero_State().root;
}

void HomeHero_Start() {
    if (!HomeHero_alive) {
        return;
    }
    HomeHero_RefreshClock(true);
    HomeHero_ApplyWeatherUi();
    StandbyClassic_AddWeatherUiListener(HomeHero_OnWeatherListener);
    if (HomeHero_State().timer != nullptr) {
        lv_timer_delete(HomeHero_State().timer);
        HomeHero_State().timer = nullptr;
    }
    HomeHero_State().timer = lv_timer_create(HomeHero_OnClockTimer, kClockTickMs, nullptr);
    StandbyClassic_RequestWeatherEnsure();
}

void HomeHero_Stop() {
    StandbyClassic_RemoveWeatherUiListener(HomeHero_OnWeatherListener);
    if (HomeHero_State().timer != nullptr) {
        lv_timer_delete(HomeHero_State().timer);
        HomeHero_State().timer = nullptr;
    }
}

void HomeHero_Teardown() {
    HomeHero_Stop();
    HomeHero_alive = false;
    HomeHero_State() = HomeHeroUiState{};
}

