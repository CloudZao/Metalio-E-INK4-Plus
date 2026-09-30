#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_weather_store.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "settings.h"
#include "wifi_station.h"

StandbyClassicUiState& StandbyClassic_State() {
    static StandbyClassicUiState s;
    return s;
}
bool StandbyClassic_alive = false;
bool StandbyClassic_as_overlay = false;
WeatherSnap StandbyClassic_weather;
char StandbyClassic_weather_day[9] = {};
std::mutex StandbyClassic_weather_mu;
TaskHandle_t StandbyClassic_fetch_task = nullptr;
std::atomic<bool> StandbyClassic_busy{false};
std::vector<TodoItem> StandbyClassic_todos;
StandbyClassicWeatherUiListener StandbyClassic_weather_listeners[kMaxWeatherListeners] = {};

const WeatherPhenom kPhenoms[] = {
    {"00", "晴", "Sunny", "Clear"},
    {"01", "多云", "Cloudy", "Cloudy"},
    {"02", "阴", "Overcast", "Overcast"},
    {"03", "阵雨", "Shower", "Shower"},
    {"04", "雷阵雨", "Thundershower", "Thundershower"},
    {"05", "雷阵雨伴有冰雹", "Thundershower with hail", "Thundershower with hail"},
    {"06", "雨夹雪", "Sleet", "Sleet"},
    {"07", "小雨", "Light rain", "Light rain"},
    {"08", "中雨", "Moderate rain", "Moderate rain"},
    {"09", "大雨", "Heavy rain", "Heavy rain"},
    {"10", "暴雨", "Storm", "Storm"},
    {"11", "大暴雨", "Heavy storm", "Heavy storm"},
    {"12", "特大暴雨", "Severe storm", "Severe storm"},
    {"13", "阵雪", "Snow flurry", "Snow flurry"},
    {"14", "小雪", "Light snow", "Light snow"},
    {"15", "中雪", "Moderate snow", "Moderate snow"},
    {"16", "大雪", "Heavy snow", "Heavy snow"},
    {"17", "暴雪", "Snowstorm", "Snowstorm"},
    {"18", "雾", "Fog", "Fog"},
    {"19", "冻雨", "Ice rain", "Ice rain"},
    {"20", "沙尘暴", "Duststorm", "Duststorm"},
    {"21", "小到中雨", "Light to moderate rain", "Light to moderate rain"},
    {"22", "中到大雨", "Moderate to heavy rain", "Moderate to heavy rain"},
    {"23", "大到暴雨", "Heavy rain to storm", "Heavy rain to storm"},
    {"24", "暴雨到大暴雨", "Storm to heavy storm", "Storm to heavy storm"},
    {"25", "大暴雨到特大暴雨", "Heavy to severe storm", "Heavy to severe storm"},
    {"26", "小到中雪", "Light to moderate snow", "Light to moderate snow"},
    {"27", "中到大雪", "Moderate to heavy snow", "Moderate to heavy snow"},
    {"28", "大到暴雪", "Heavy snow to snowstorm", "Heavy snow to snowstorm"},
    {"29", "浮尘", "Dust", "Dust"},
    {"30", "扬沙", "Sand", "Sand"},
    {"31", "强沙尘暴", "Sandstorm", "Sandstorm"},
    {"32", "浓雾", "Dense fog", "Dense fog"},
    {"33", "龙卷风", "Tornado", "Tornado"},
    {"34", "弱高吹雪", "Weak high blow snow", "Weak high blow snow"},
    {"35", "轻雾", "Mist", "Mist"},
    {"49", "强浓雾", "Heavy dense fog", "Heavy dense fog"},
    {"53", "霾", "Haze", "Haze"},
    {"54", "中度霾", "Moderate haze", "Moderate haze"},
    {"55", "重度霾", "Severe haze", "Severe haze"},
    {"56", "严重霾", "Hazardous haze", "Hazardous haze"},
    {"57", "大雾", "Heavy fog", "Heavy fog"},
    {"58", "特强浓雾", "Extra-heavy dense fog", "Extra-heavy dense fog"},
    {"301", "雨", "Rain", "Rain"},
    {"302", "雪", "Snow", "Snow"},
};

const lv_font_t* StandbyClassic_WeatherFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

const lv_font_t* StandbyClassic_TodoFont() {
    return fontpack_lv_font_ui();
}

bool StandbyClassic_CodeKnown(const char* code) {
    if (code == nullptr || code[0] == '\0') {
        return false;
    }
    for (const auto& p : kPhenoms) {
        if (std::strcmp(p.code, code) == 0) {
            return true;
        }
    }
    return false;
}

const char* StandbyClassic_MapTextToCode(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }
    for (const auto& p : kPhenoms) {
        if (std::strcmp(text, p.zh) == 0 || std::strcmp(text, p.en_day) == 0 ||
            std::strcmp(text, p.en_night) == 0) {
            return p.code;
        }
    }
    return nullptr;
}

const WeatherPhenom* StandbyClassic_FindPhenomByCode(const char* code) {
    if (code == nullptr || code[0] == '\0') {
        return nullptr;
    }
    for (const auto& p : kPhenoms) {
        if (std::strcmp(p.code, code) == 0) {
            return &p;
        }
    }
    return nullptr;
}

const char* StandbyClassic_LocalizedPhenomText(const WeatherSnap& snap) {
    const WeatherPhenom* p = StandbyClassic_FindPhenomByCode(snap.icon_code);
    if (p == nullptr && snap.text[0] != '\0') {
        if (const char* code = StandbyClassic_MapTextToCode(snap.text)) {
            p = StandbyClassic_FindPhenomByCode(code);
        }
    }
    if (p == nullptr) {
        return snap.text;
    }
    return (std::strcmp(Lang::CODE, "zh-CN") == 0) ? p->zh : p->en_day;
}

const char* StandbyClassic_WeekdayShortName(int wday) {
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

void StandbyClassic_FormatPhenomCode(char* out, size_t out_len, int n) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    const unsigned v = static_cast<unsigned>(n < 0 ? 0 : n) % 1000u;
    if (v >= 100u) {
        std::snprintf(out, out_len, "%u", v);
    } else {
        std::snprintf(out, out_len, "%02u", v);
    }
}

bool StandbyClassic_FormatToday(char* out, size_t out_len) {
    if (out == nullptr || out_len < 9) {
        return false;
    }
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < (2020 - 1900)) {
        out[0] = '\0';
        return false;
    }
    const unsigned year = static_cast<unsigned>(tm_info.tm_year + 1900) % 10000u;
    const unsigned month = static_cast<unsigned>(tm_info.tm_mon + 1) % 100u;
    const unsigned day = static_cast<unsigned>(tm_info.tm_mday) % 100u;
    std::snprintf(out, out_len, "%04u%02u%02u", year, month, day);
    return true;
}

bool StandbyClassic_WeatherRamIsToday() {
    char today[9];
    if (!StandbyClassic_FormatToday(today, sizeof(today))) {
        return false;
    }
    std::lock_guard<std::mutex> lock(StandbyClassic_weather_mu);
    return StandbyClassic_weather.valid && std::strcmp(StandbyClassic_weather_day, today) == 0;
}

WeatherSnap StandbyClassic_WeatherRamCopy() {
    std::lock_guard<std::mutex> lock(StandbyClassic_weather_mu);
    return StandbyClassic_weather;
}

void StandbyClassic_WeatherRamStore(const WeatherSnap& snap, const char* day) {
    std::lock_guard<std::mutex> lock(StandbyClassic_weather_mu);
    StandbyClassic_weather = snap;
    strlcpy(StandbyClassic_weather_day, day != nullptr ? day : "", sizeof(StandbyClassic_weather_day));
}

bool StandbyClassic_LoadWeatherFromNvs(WeatherSnap& out, char* day_out, size_t day_len) {
    out = WeatherSnap{};
    if (day_out != nullptr && day_len > 0) {
        day_out[0] = '\0';
    }
    Settings settings(kWeatherNs, false);
    const std::string day = settings.GetString("day");
    if (day.size() != 8) {
        return false;
    }
    if (day_out != nullptr && day_len > 0) {
        strlcpy(day_out, day.c_str(), day_len);
    }
    const std::string text = settings.GetString("text");
    const std::string icon = settings.GetString("icon");
    const std::string lunar = settings.GetString("lunar");
    strlcpy(out.text, text.c_str(), sizeof(out.text));
    strlcpy(out.icon_code, icon.c_str(), sizeof(out.icon_code));
    strlcpy(out.lunar, lunar.c_str(), sizeof(out.lunar));
    out.has_temp = settings.GetBool("ht", false);
    out.temp = settings.GetInt("temp", 0);
    out.valid = (out.text[0] != '\0') || out.has_temp;
    if (out.valid && !settings.GetBool("lunar_ok", false)) {
        return false;
    }
    return out.valid;
}

void StandbyClassic_SaveWeatherToNvs(const WeatherSnap& snap, const char* day) {
    if (day == nullptr || day[0] == '\0' || !snap.valid) {
        return;
    }
    Settings settings(kWeatherNs, true);
    settings.SetString("day", day);
    settings.SetString("text", snap.text);
    settings.SetString("icon", snap.icon_code);
    settings.SetString("lunar", snap.lunar);
    settings.SetInt("temp", snap.temp);
    settings.SetBool("ht", snap.has_temp);
    settings.SetBool("lunar_ok", true);
    ESP_LOGI(TAG, "weather nvs saved day=%s text=%s temp=%d icon=%s lunar=%s", day, snap.text,
             snap.temp, snap.icon_code, snap.lunar);
}

