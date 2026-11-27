#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include "infra/tasks/task_create.h"
#include "weather_cache.h"
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "infra/storage/settings.h"

static const char* TAG = "Weather";
static constexpr int kHttpTimeoutMs = 15000;
static constexpr const char* kWeatherNs = "weather";
static WeatherSnap s_cache;
static char s_cache_day[9] = {};
static std::mutex s_cache_mutex;
static std::mutex s_fetch_mutex;
static std::atomic<bool> s_refresh_requested{false};
static void (*s_refresh_completed)() = nullptr;

static const WeatherPhenom kPhenoms[] = {
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

static void WeatherStoreCache(const WeatherSnap& snap, const char* day) {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_cache = snap;
    strlcpy(s_cache_day, day != nullptr ? day : "", sizeof(s_cache_day));
}

static bool WeatherLoadNvs(WeatherSnap& out, char* day_out, size_t day_len) {
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

static void WeatherSaveNvs(const WeatherSnap& snap, const char* day) {
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
    if (!settings.Commit()) {
        ESP_LOGW(TAG, "operation=save_weather_cache result=storage_error; RAM retained");
    }
}

static bool WeatherParseJson(const std::string& body, WeatherSnap& out, std::string& err) {
    out = WeatherSnap{};
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = LangString_STANDBY_PARSE_FAIL;
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err = (cJSON_IsString(msg) && msg->valuestring != nullptr) ? msg->valuestring : LangString_STANDBY_WEATHER_FAIL;
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        err = LangString_STANDBY_NO_DATA;
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
        WeatherFormatCode(num_buf, sizeof(num_buf), phen->valueint);
        api_code = num_buf;
    }
    if (WeatherCodeKnown(api_code)) {
        strlcpy(out.icon_code, api_code, sizeof(out.icon_code));
    } else if (const char* mapped = WeatherMapTextToCode(out.text)) {
        strlcpy(out.icon_code, mapped, sizeof(out.icon_code));
    }

    out.valid = (out.text[0] != '\0') || out.has_temp;
    cJSON_Delete(root);
    if (!out.valid) {
        err = LangString_STANDBY_NO_WEATHER;
        return false;
    }
    return true;
}

static bool WeatherHttpGet(std::string& body_out, std::string& err_out) {
    body_out.clear();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = LangString_STANDBY_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = LangString_STANDBY_CONN_FAIL;
        return false;
    }
    const std::string url = ApiWeatherLatestUrl();
    if (url.empty()) {
        err_out = "weather api not configured";
        ESP_LOGI(TAG, "skip weather: cloud endpoints blank");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    ApiApplyCommonHeaders(http);
    if (!http->Open("GET", url)) {
        err_out = LangString_STANDBY_REQUEST_FAIL;
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        err_out = LangString_STANDBY_REQUEST_FAIL;
        return false;
    }
    return true;
}

bool WeatherCacheIsToday() {
    char today[9];
    if (!WeatherFormatToday(today, sizeof(today))) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    return s_cache.valid && std::strcmp(s_cache_day, today) == 0;
}

WeatherSnap WeatherCacheSnapshot() {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    return s_cache;
}

void WeatherEnsureCached() {
    std::lock_guard<std::mutex> request(s_fetch_mutex);
    if (WeatherCacheIsToday()) {
        return;
    }
    char today[9];
    if (!WeatherFormatToday(today, sizeof(today))) {
        return;
    }
    WeatherSnap snapshot;
    char saved_day[9] = {};
    if (WeatherLoadNvs(snapshot, saved_day, sizeof(saved_day)) && !std::strcmp(saved_day, today)) {
        WeatherStoreCache(snapshot, today);
        return;
    }
    if (!Board::GetInstance().IsNetworkReady()) {
        return;
    }
    std::string body, error;
    if (!WeatherHttpGet(body, error)) {
        ESP_LOGW(TAG, "operation=refresh stage=http result=failed; cache retained");
        return;
    }
    if (!WeatherParseJson(body, snapshot, error)) {
        ESP_LOGE(TAG, "operation=refresh stage=response result=invalid; cache retained");
        return;
    }
    WeatherSaveNvs(snapshot, today);
    WeatherStoreCache(snapshot, today);
}

bool WeatherCodeKnown(const char* code) {
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

const char* WeatherMapTextToCode(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }
    for (const auto& p : kPhenoms) {
        if (std::strcmp(text, p.zh) == 0 || std::strcmp(text, p.en_day) == 0 || std::strcmp(text, p.en_night) == 0) {
            return p.code;
        }
    }
    return nullptr;
}

const WeatherPhenom* WeatherFindPhenom(const char* code) {
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

void WeatherFormatCode(char* out, size_t out_len, int n) {
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

bool WeatherFormatToday(char* out, size_t out_len) {
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

static void WeatherRefreshTask(void*) {
    const auto completed = s_refresh_completed;
    try {
        WeatherEnsureCached();
    } catch (const std::bad_alloc&) {
        ESP_LOGW(TAG, "operation=weather_refresh stage=fetch result=no_memory");
    }
    s_refresh_requested.store(false, std::memory_order_release);
    if (completed != nullptr) {
        completed();
    }
    vTaskDelete(nullptr);
}

bool WeatherRequestEnsure(void (*completed)()) {
    if (WeatherCacheIsToday() || s_refresh_requested.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    s_refresh_completed = completed;
    if (FirmwareCreateTask(WeatherRefreshTask, "weather_refresh", 10 * 1024, nullptr,
                                tskIDLE_PRIORITY + 2, nullptr, 0, 0) != pdPASS) {
        s_refresh_requested.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "operation=weather_refresh stage=create_task result=no_memory");
        return false;
    }
    return true;
}
