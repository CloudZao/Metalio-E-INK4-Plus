#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include "ui/common/widgets/weather_view.h"
#include "assets/lang_config.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr const char* TAG = "WeatherView";
static constexpr int kWeatherListenerCount = 2;
static WeatherUiListener s_weather_listeners[kWeatherListenerCount] = {};

static void NotifyWeatherListeners() {
    for (int i = 0; i < kWeatherListenerCount; ++i) {
        if (s_weather_listeners[i] != nullptr) {
            s_weather_listeners[i]();
        }
    }
}

bool WeatherView_Copy(WeatherUiView* out) {
    if (out == nullptr) {
        return false;
    }
    *out = WeatherUiView{};
    const WeatherSnap snap = WeatherCacheSnapshot();
    if (!snap.valid) {
        return false;
    }
    out->valid = true;
    out->has_temp = snap.has_temp;
    out->temp = snap.temp;
    strlcpy(out->text, WeatherView_LocalizedPhenomText(snap), sizeof(out->text));
    strlcpy(out->icon_code, snap.icon_code, sizeof(out->icon_code));
    strlcpy(out->lunar, snap.lunar, sizeof(out->lunar));
    return true;
}

void WeatherView_AddListener(WeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kWeatherListenerCount; ++i) {
        if (s_weather_listeners[i] == cb) {
            return;
        }
    }
    for (int i = 0; i < kWeatherListenerCount; ++i) {
        if (s_weather_listeners[i] == nullptr) {
            s_weather_listeners[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "weather listener slots full");
}

void WeatherView_RemoveListener(WeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kWeatherListenerCount; ++i) {
        if (s_weather_listeners[i] == cb) {
            s_weather_listeners[i] = nullptr;
        }
    }
}

void WeatherView_RequestEnsure() {
    WeatherRequestEnsure(WeatherView_Refresh);
}

void WeatherView_FormatLunarOrWeekday(char* out, size_t out_len, const char* lunar_zh) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (std::strcmp(LangCode, "zh-CN") == 0) {
        if (lunar_zh != nullptr) {
            strlcpy(out, lunar_zh, out_len);
        }
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr) {
        return;
    }
    strlcpy(out, WeatherView_WeekdayShortName(tm_info.tm_wday), out_len);
}

const char* WeatherView_LocalizedPhenomText(const WeatherSnap& snap) {
    const WeatherPhenom* p = WeatherFindPhenom(snap.icon_code);
    if (p == nullptr && snap.text[0] != '\0') {
        if (const char* code = WeatherMapTextToCode(snap.text)) {
            p = WeatherFindPhenom(code);
        }
    }
    if (p == nullptr) {
        return snap.text;
    }
    return (std::strcmp(LangCode, "zh-CN") == 0) ? p->zh : p->en_day;
}

void WeatherView_Refresh() {
    if (!ScreenLvAsync([](void*) { NotifyWeatherListeners(); })) {
        ESP_LOGW(TAG, "operation=weather_refresh stage=ui_enqueue result=full");
    }
}

const char* WeatherView_WeekdayShortName(int wday) {
    switch (wday) {
        case 0:
            return LangString_HOME_WDAY_SUN;
        case 1:
            return LangString_HOME_WDAY_MON;
        case 2:
            return LangString_HOME_WDAY_TUE;
        case 3:
            return LangString_HOME_WDAY_WED;
        case 4:
            return LangString_HOME_WDAY_THU;
        case 5:
            return LangString_HOME_WDAY_FRI;
        case 6:
            return LangString_HOME_WDAY_SAT;
        default:
            return "";
    }
}
