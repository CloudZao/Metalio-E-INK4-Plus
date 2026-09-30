#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_weather_fetch.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "wifi_station.h"

void StandbyClassic_NotifyWeatherListeners() {
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] != nullptr) {
            StandbyClassic_weather_listeners[i]();
        }
    }
}

static void AsyncApplyWeatherUi(void* /*p*/) {
    StandbyClassic_ApplyWeatherUi();
    StandbyClassic_NotifyWeatherListeners();
}

void StandbyClassic_PostWeatherUi() {
    if (lv_async_call(AsyncApplyWeatherUi, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call weather ui failed");
    }
}

bool StandbyClassic_ParseWeatherJson(const std::string& body, WeatherSnap& out, std::string& err) {
    out = WeatherSnap{};
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = Lang::Strings::STANDBY_PARSE_FAIL;
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err = (cJSON_IsString(msg) && msg->valuestring != nullptr) ? msg->valuestring
                                                                   : Lang::Strings::STANDBY_WEATHER_FAIL;
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        err = Lang::Strings::STANDBY_NO_DATA;
        cJSON_Delete(root);
        return false;
    }

    cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "text");
    if (cJSON_IsString(text) && text->valuestring != nullptr) {
        strlcpy(out.text, text->valuestring, sizeof(out.text));
    }

    cJSON* lunar = cJSON_GetObjectItemCaseSensitive(data, "lunar");
    if (cJSON_IsString(lunar) && lunar->valuestring != nullptr) {
        strlcpy(out.lunar, lunar->valuestring, sizeof(out.lunar));
    }

    cJSON* temp = cJSON_GetObjectItemCaseSensitive(data, "temp");
    if (cJSON_IsNumber(temp)) {
        out.temp = temp->valueint;
        out.has_temp = true;
    }

    char num_buf[8] = {};
    const char* api_code = nullptr;
    cJSON* phen = cJSON_GetObjectItemCaseSensitive(data, "phenomena");
    if (phen == nullptr) {
        phen = cJSON_GetObjectItemCaseSensitive(data, "phenomenon");
    }
    if (cJSON_IsString(phen) && phen->valuestring != nullptr) {
        api_code = phen->valuestring;
    } else if (cJSON_IsNumber(phen)) {
        StandbyClassic_FormatPhenomCode(num_buf, sizeof(num_buf), phen->valueint);
        api_code = num_buf;
    }
    if (StandbyClassic_CodeKnown(api_code)) {
        strlcpy(out.icon_code, api_code, sizeof(out.icon_code));
    } else if (const char* mapped = StandbyClassic_MapTextToCode(out.text)) {
        strlcpy(out.icon_code, mapped, sizeof(out.icon_code));
    }

    out.valid = (out.text[0] != '\0') || out.has_temp;
    cJSON_Delete(root);
    if (!out.valid) {
        err = Lang::Strings::STANDBY_NO_WEATHER;
        return false;
    }
    return true;
}

bool StandbyClassic_HttpGetWeather(std::string& body_out, std::string& err_out) {
    body_out.clear();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::STANDBY_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::STANDBY_CONN_FAIL;
        return false;
    }
    const std::string url = api::WeatherLatestUrl();
    if (url.empty()) {
        err_out = "weather api not configured";
        ESP_LOGI(TAG, "skip weather: cloud endpoints blank");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    api::ApplyCommonHeaders(http);
    api::LogHttpRequest(TAG, "GET", url);
    if (!http->Open("GET", url)) {
        err_out = Lang::Strings::STANDBY_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(body_out));
    if (status < 200 || status >= 300) {
        err_out = Lang::Strings::STANDBY_REQUEST_FAIL;
        return false;
    }
    return true;
}

void StandbyClassic_EnsureWeatherCachedLocked() {
    char today[9];
    if (!StandbyClassic_FormatToday(today, sizeof(today))) {
        ESP_LOGW(TAG, "weather skip: time not ready");
        return;
    }
    if (StandbyClassic_WeatherRamIsToday()) {
        ESP_LOGI(TAG, "weather ram hit day=%s", today);
        return;
    }

    WeatherSnap snap;
    char nvs_day[9] = {};
    if (StandbyClassic_LoadWeatherFromNvs(snap, nvs_day, sizeof(nvs_day)) && std::strcmp(nvs_day, today) == 0) {
        StandbyClassic_WeatherRamStore(snap, today);
        ESP_LOGI(TAG, "weather nvs hit day=%s text=%s temp=%d", today, snap.text, snap.temp);
        return;
    }

    if (StandbyClassic_busy.exchange(true)) {
        ESP_LOGI(TAG, "weather http already in flight");
        return;
    }

    bool done = false;
    if (StandbyClassic_WeatherRamIsToday()) {
        done = true;
    } else if (StandbyClassic_LoadWeatherFromNvs(snap, nvs_day, sizeof(nvs_day)) &&
               std::strcmp(nvs_day, today) == 0) {
        StandbyClassic_WeatherRamStore(snap, today);
        ESP_LOGI(TAG, "weather nvs hit day=%s text=%s temp=%d", today, snap.text, snap.temp);
        done = true;
    } else if (!WifiStation::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "weather skip http (wifi down)");
        done = true;
    }

    if (!done) {
        std::string body;
        std::string err;
        if (!StandbyClassic_HttpGetWeather(body, err)) {
            ESP_LOGW(TAG, "weather http fail: %s", err.c_str());
        } else if (!StandbyClassic_ParseWeatherJson(body, snap, err)) {
            ESP_LOGW(TAG, "weather parse fail: %s", err.c_str());
        } else {
            StandbyClassic_SaveWeatherToNvs(snap, today);
            StandbyClassic_WeatherRamStore(snap, today);
        }
    }
    StandbyClassic_busy.store(false);
}

static void EnsureWeatherTask(void* /*arg*/) {
    StandbyClassic_EnsureWeatherCachedLocked();
    StandbyClassic_fetch_task = nullptr;
    StandbyClassic_PostWeatherUi();
    vTaskDelete(nullptr);
}

void StandbyClassic_ScheduleWeatherEnsure() {
    if (StandbyClassic_WeatherRamIsToday()) {
        return;
    }
    if (StandbyClassic_fetch_task != nullptr || StandbyClassic_busy.load()) {
        return;
    }
    if (xTaskCreatePinnedToCore(EnsureWeatherTask, "standby_wx", kWorkerStack, nullptr,
                                tskIDLE_PRIORITY + 2, &StandbyClassic_fetch_task, 0) != pdPASS) {
        StandbyClassic_fetch_task = nullptr;
        ESP_LOGW(TAG, "weather task create fail");
    }
}


void StandbyClassic_EnsureWeatherCached() {
    StandbyClassic_EnsureWeatherCachedLocked();
    StandbyClassic_PostWeatherUi();
}

bool StandbyClassic_CopyWeatherView(StandbyClassicWeatherView* out) {
    if (out == nullptr) {
        return false;
    }
    *out = StandbyClassicWeatherView{};
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    if (!snap.valid) {
        return false;
    }
    out->valid = true;
    out->has_temp = snap.has_temp;
    out->temp = snap.temp;
    strlcpy(out->text, StandbyClassic_LocalizedPhenomText(snap), sizeof(out->text));
    strlcpy(out->icon_code, snap.icon_code, sizeof(out->icon_code));
    strlcpy(out->lunar, snap.lunar, sizeof(out->lunar));
    return true;
}

void StandbyClassic_FormatLunarOrWeekday(char* out, size_t out_len, const char* lunar_zh) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (std::strcmp(Lang::CODE, "zh-CN") == 0) {
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
    strlcpy(out, StandbyClassic_WeekdayShortName(tm_info.tm_wday), out_len);
}

void StandbyClassic_AddWeatherUiListener(StandbyClassicWeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == cb) {
            return;
        }
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == nullptr) {
            StandbyClassic_weather_listeners[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "weather listener slots full");
}

void StandbyClassic_RemoveWeatherUiListener(StandbyClassicWeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == cb) {
            StandbyClassic_weather_listeners[i] = nullptr;
        }
    }
}

void StandbyClassic_RequestWeatherEnsure() {
    StandbyClassic_ScheduleWeatherEnsure();
}

