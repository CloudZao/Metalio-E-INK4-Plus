#include "pcf8563.h"

#include <sys/time.h>
#include <esp_log.h>

#define TAG "Pcf8563"

namespace {
constexpr uint8_t kRegCtrl1 = 0x00;
constexpr uint8_t kRegCtrl2 = 0x01;
constexpr uint8_t kRegSeconds = 0x02; // bit7 = VL
constexpr uint8_t kBitVl = 0x80;
constexpr uint8_t kBitAf = 0x08; // Control_status_2 alarm flag
constexpr uint8_t kBitTf = 0x04; // Control_status_2 timer flag
constexpr int     kI2cTimeoutMs = 100;
constexpr int     kProbeTimeoutMs = 100;
} // namespace

uint8_t Pcf8563::DecToBcd(uint8_t v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

uint8_t Pcf8563::BcdToDec(uint8_t v) {
    return static_cast<uint8_t>(((v >> 4) * 10) + (v & 0x0F));
}

bool Pcf8563::Begin(const power_i2c_t* bus, uint8_t addr) {
    if (bus == nullptr) {
        ESP_LOGW(TAG, "Begin() null bus");
        return false;
    }
    bus_ = *bus;
    bus_valid_ = true;
    addr_ = addr;
    if (ready_) {
        return true;
    }

    esp_err_t probe = power_i2c_probe(&bus_, addr_, kProbeTimeoutMs);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 @0x%02X probe failed: %s", addr_, esp_err_to_name(probe));
        return false;
    }

    ready_ = true;

    // 清 STOP，保证振荡器运行
    uint8_t ctrl1 = 0x00;
    if (!WriteRegs(kRegCtrl1, &ctrl1, 1)) {
        ESP_LOGW(TAG, "clear CTRL1 failed");
    }

    struct tm now_tm = {};
    bool valid = false;
    if (GetTime(now_tm, &valid)) {
        ESP_LOGI(TAG, "PCF8563 online @0x%02X %04d-%02d-%02d %02d:%02d:%02d VL=%s", addr_,
                 now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour,
                 now_tm.tm_min, now_tm.tm_sec, valid ? "ok" : "set");
    } else {
        ESP_LOGW(TAG, "PCF8563 ACK 通过但读时间失败");
    }
    return true;
}

bool Pcf8563::WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
    if (!ready_ || !bus_valid_ || data == nullptr || len == 0 || len > 16) {
        return false;
    }
    return power_i2c_write_reg(&bus_, addr_, reg, data, len, kI2cTimeoutMs) == ESP_OK;
}

bool Pcf8563::ReadRegs(uint8_t reg, uint8_t* data, size_t len) {
    if (!ready_ || !bus_valid_ || data == nullptr || len == 0) {
        return false;
    }
    return power_i2c_write_read(&bus_, addr_, reg, data, len, kI2cTimeoutMs) == ESP_OK;
}

bool Pcf8563::GetTime(struct tm& out, bool* valid) {
    uint8_t raw[7] = {};
    if (!ReadRegs(kRegSeconds, raw, sizeof(raw))) {
        return false;
    }

    const bool vl_ok = (raw[0] & kBitVl) == 0;
    if (valid) {
        *valid = vl_ok;
    }

    out = {};
    out.tm_sec = BcdToDec(raw[0] & 0x7F);
    out.tm_min = BcdToDec(raw[1] & 0x7F);
    out.tm_hour = BcdToDec(raw[2] & 0x3F);
    out.tm_mday = BcdToDec(raw[3] & 0x3F);
    out.tm_wday = BcdToDec(raw[4] & 0x07);
    out.tm_mon = BcdToDec(raw[5] & 0x1F) - 1;
    // PCF8563 year：00..99；与 BM8563 用法一致：基准 2000
    const int year_xx = BcdToDec(raw[6]);
    out.tm_year = 100 + year_xx;
    out.tm_isdst = -1;
    return true;
}

bool Pcf8563::SetTime(const struct tm& in) {
    if (!ready_) {
        return false;
    }

    int year = in.tm_year + 1900;
    if (year < 2000 || year > 2099) {
        ESP_LOGW(TAG, "year %d out of PCF8563 range 2000-2099", year);
        return false;
    }

    uint8_t raw[7] = {
        DecToBcd(static_cast<uint8_t>(in.tm_sec)),
        DecToBcd(static_cast<uint8_t>(in.tm_min)),
        DecToBcd(static_cast<uint8_t>(in.tm_hour)),
        DecToBcd(static_cast<uint8_t>(in.tm_mday)),
        DecToBcd(static_cast<uint8_t>(in.tm_wday % 7)),
        DecToBcd(static_cast<uint8_t>(in.tm_mon + 1)),
        DecToBcd(static_cast<uint8_t>(year - 2000)),
    };
    return WriteRegs(kRegSeconds, raw, sizeof(raw));
}

bool Pcf8563::ApplyRtcToSystem() {
    struct tm rtc_tm = {};
    bool valid = false;
    if (!GetTime(rtc_tm, &valid)) {
        return false;
    }
    if (!valid) {
        ESP_LOGW(TAG, "RTC VL set, skip ApplyRtcToSystem (time untrusted)");
        return false;
    }
    // 粗检：避免把损坏寄存器灌进系统
    if (rtc_tm.tm_mon < 0 || rtc_tm.tm_mon > 11 || rtc_tm.tm_mday < 1 || rtc_tm.tm_mday > 31 ||
        rtc_tm.tm_hour > 23 || rtc_tm.tm_min > 59 || rtc_tm.tm_sec > 59) {
        ESP_LOGW(TAG, "RTC calendar out of range, skip apply");
        return false;
    }

    time_t ts = mktime(&rtc_tm);
    if (ts < 0) {
        ESP_LOGW(TAG, "mktime failed");
        return false;
    }

    struct timeval tv = {
        .tv_sec = ts,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return false;
    }
    ESP_LOGI(TAG, "system time <- RTC %04d-%02d-%02d %02d:%02d:%02d", rtc_tm.tm_year + 1900,
             rtc_tm.tm_mon + 1, rtc_tm.tm_mday, rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
    return true;
}

bool Pcf8563::SyncSystemToRtc() {
    time_t now = time(nullptr);
    struct tm local = {};
    if (localtime_r(&now, &local) == nullptr) {
        return false;
    }
    if (!SetTime(local)) {
        return false;
    }
    ESP_LOGI(TAG, "RTC <- system %04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900,
             local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return true;
}

bool Pcf8563::ClearIrqFlags() {
    uint8_t ctrl2 = 0;
    if (!ReadRegs(kRegCtrl2, &ctrl2, 1)) {
        return false;
    }
    ctrl2 = static_cast<uint8_t>(ctrl2 & static_cast<uint8_t>(~(kBitAf | kBitTf)));
    return WriteRegs(kRegCtrl2, &ctrl2, 1);
}
