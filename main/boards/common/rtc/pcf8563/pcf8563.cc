#include <cerrno>
#include <esp_log.h>
#include <sys/time.h>
#include "pcf8563.h"

#define TAG "Pcf8563"

static constexpr uint8_t kRegCtrl1 = 0x00;
static constexpr uint8_t kRegCtrl2 = 0x01;
static constexpr uint8_t kRegSeconds = 0x02;  // bit7 = VL
static constexpr uint8_t kBitVl = 0x80;
static constexpr uint8_t kBitAf = 0x08;  // Control_status_2 alarm flag
static constexpr uint8_t kBitTf = 0x04;  // Control_status_2 timer flag
static constexpr int kI2cTimeoutMs = 100;
static constexpr int kProbeTimeoutMs = 100;

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
        DecToBcd(static_cast<uint8_t>(in.tm_sec)),      DecToBcd(static_cast<uint8_t>(in.tm_min)),
        DecToBcd(static_cast<uint8_t>(in.tm_hour)),     DecToBcd(static_cast<uint8_t>(in.tm_mday)),
        DecToBcd(static_cast<uint8_t>(in.tm_wday % 7)), DecToBcd(static_cast<uint8_t>(in.tm_mon + 1)),
        DecToBcd(static_cast<uint8_t>(year - 2000)),
    };
    return WriteRegs(kRegSeconds, raw, sizeof(raw));
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

    uint8_t initial_ctrl[2] = {};
    if (ReadRegs(kRegCtrl1, initial_ctrl, sizeof(initial_ctrl))) {
        ESP_LOGI(TAG, "boot CTRL1=0x%02X CTRL2=0x%02X STOP=%d", initial_ctrl[0], initial_ctrl[1],
                 (initial_ctrl[0] & 0x20) != 0);
    }
    // 清 STOP，保证振荡器运行
    uint8_t ctrl1 = 0x00;
    if (!WriteRegs(kRegCtrl1, &ctrl1, 1)) {
        ESP_LOGW(TAG, "clear CTRL1 failed");
    }

    struct tm now_tm = {};
    bool valid = false;
    if (GetTime(now_tm, &valid)) {
        ESP_LOGI(TAG, "PCF8563 online @0x%02X %04d-%02d-%02d %02d:%02d:%02d VL=%s", addr_, now_tm.tm_year + 1900,
                 now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec, valid ? "ok" : "set");
    } else {
        ESP_LOGW(TAG, "PCF8563 ACK 通过但读时间失败");
    }
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

bool Pcf8563::WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
    if (!ready_ || !bus_valid_ || data == nullptr || len == 0 || len > 16) {
        return false;
    }
    const esp_err_t err = power_i2c_write_reg(&bus_, addr_, reg, data, len, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC write addr=0x%02X reg=0x%02X len=%u failed: %s", addr_, reg, static_cast<unsigned>(len),
                 esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool Pcf8563::ReadRegs(uint8_t reg, uint8_t* data, size_t len) {
    if (!ready_ || !bus_valid_ || data == nullptr || len == 0) {
        return false;
    }
    const esp_err_t err = power_i2c_write_read(&bus_, addr_, reg, data, len, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC read addr=0x%02X reg=0x%02X len=%u failed: %s", addr_, reg, static_cast<unsigned>(len),
                 esp_err_to_name(err));
    }
    return err == ESP_OK;
}

uint8_t Pcf8563::DecToBcd(uint8_t v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

uint8_t Pcf8563::BcdToDec(uint8_t v) {
    return static_cast<uint8_t>(((v >> 4) * 10) + (v & 0x0F));
}
