#include <cerrno>
#include <esp_log.h>
#include <sys/time.h>
#include "time_service.h"
#include "boards/common/rtc/pcf8563/pcf8563.h"
#include "infra/network/time/ntp_server.h"
#include "infra/storage/settings.h"

static const char* TAG = "TimeService";

bool TimeServiceRestoreRtc() {
    auto& rtc = Pcf8563::GetInstance();
    struct tm rtc_tm = {};
    bool valid = false;
    if (!rtc.GetTime(rtc_tm, &valid)) {
        ESP_LOGW(TAG, "RTC -> system failed at calendar read (ready=%d)", rtc.IsReady());
        return false;
    }
    if (!valid) {
        ESP_LOGW(TAG, "RTC VL set, skip ApplyRtcToSystem (time untrusted)");
        return false;
    }
    ESP_LOGI(TAG, "RTC -> system candidate %04d-%02d-%02d %02d:%02d:%02d wday=%d", rtc_tm.tm_year + 1900,
             rtc_tm.tm_mon + 1, rtc_tm.tm_mday, rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec, rtc_tm.tm_wday);
    // 粗检：避免把损坏寄存器灌进系统
    if (rtc_tm.tm_mon < 0 || rtc_tm.tm_mon > 11 || rtc_tm.tm_mday < 1 || rtc_tm.tm_mday > 31 || rtc_tm.tm_hour > 23 ||
        rtc_tm.tm_min > 59 || rtc_tm.tm_sec > 59) {
        ESP_LOGW(TAG, "RTC calendar out of range, skip apply");
        return false;
    }

    time_t ts = mktime(&rtc_tm);
    if (ts < 0) {
        ESP_LOGW(TAG, "RTC -> system mktime failed errno=%d", errno);
        return false;
    }
    // 旧固件保存当地日历；新格式保存 UTC 日历，用标记区分升级前的数据。
    Settings clock_settings("clock");
    if (clock_settings.GetBool("rtc_utc")) {
        ts += NtpServer::GetInstance().UtcOffset();
    }

    struct timeval tv = {
        .tv_sec = ts,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(TAG, "RTC -> system settimeofday failed errno=%d epoch=%lld", errno, static_cast<long long>(ts));
        return false;
    }
    ESP_LOGI(TAG, "RTC apply readback epoch=%lld expected=%lld", static_cast<long long>(time(nullptr)),
             static_cast<long long>(ts));
    ESP_LOGI(TAG, "system time <- RTC %04d-%02d-%02d %02d:%02d:%02d", rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1,
             rtc_tm.tm_mday, rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
    return true;
}

bool TimeServiceStoreRtc() {
    auto& rtc = Pcf8563::GetInstance();
    time_t now = time(nullptr);
    struct tm utc = {};
    if (gmtime_r(&now, &utc) == nullptr) {
        ESP_LOGE(TAG, "system -> RTC UTC conversion failed");
        return false;
    }
    if (!rtc.SetTime(utc)) {
        ESP_LOGW(TAG, "system -> RTC calendar write failed (ready=%d)", rtc.IsReady());
        return false;
    }
    Settings settings("clock", true);
    if (!settings.GetBool("rtc_utc") && (!settings.SetBool("rtc_utc", true) || !settings.Commit()))
        return false;
    ESP_LOGI(TAG, "RTC <- system UTC %04d-%02d-%02d %02d:%02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
    return true;
}
