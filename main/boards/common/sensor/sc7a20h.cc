#include "sc7a20h.h"

#include <esp_log.h>

#define TAG "Sc7a20h"

namespace {
constexpr uint8_t kRegWhoAmI = 0x0F;
constexpr uint8_t kRegCtrlReg1 = 0x20;
constexpr uint8_t kRegCtrlReg2 = 0x21;
constexpr uint8_t kRegCtrlReg3 = 0x22;
constexpr uint8_t kRegCtrlReg4 = 0x23;
constexpr uint8_t kRegCtrlReg5 = 0x24;
constexpr uint8_t kRegCtrlReg6 = 0x25;
constexpr uint8_t kRegOutXL = 0x28;
constexpr uint8_t kRegAoi1Cfg = 0x30;
constexpr uint8_t kRegAoi1Src = 0x31;
constexpr uint8_t kRegAoi1Ths = 0x32;
constexpr uint8_t kRegAoi1Dur = 0x33;
constexpr uint8_t kAutoIncMask = 0x80;
// CTRL_REG1=0x57：ODR=100Hz，XYZ 使能；CTRL_REG4=0x88：BDU+HR，±2g
constexpr uint8_t kCtrlReg1Val = 0x57;
constexpr uint8_t kCtrlReg2Val = 0x01; // HPIS1：AOI1 用高通后数据，活动检测不受重力常偏
constexpr uint8_t kCtrlReg3Val = 0x40; // I1_AOI1 → INT1
constexpr uint8_t kCtrlReg4Val = 0x88;
constexpr uint8_t kCtrlReg5Val = 0x08; // LIR_INT1：锁存至读 AOI1_SRC
// CTRL_REG6：INT_PP_OD(bit0)=1 开漏 + H_LACTIVE(bit1)=1 低有效，与 TCA INT 线与
constexpr uint8_t kCtrlReg6Val = 0x03;
constexpr uint8_t kAoi1CfgVal = 0x2A; // OR：XHIE|YHIE|ZHIE
constexpr uint8_t kAoi1ThsVal = 0x10; // ±2g 约 16mg/LSB → ~256mg
constexpr uint8_t kAoi1DurVal = 0x00;
constexpr uint8_t kAoi1IaMask = 0x40; // AOI1_SRC.IA
constexpr float   kMgPerLsb = 1.0f;
constexpr int     kI2cTimeoutMs = 100;
constexpr int     kProbeTimeoutMs = 100;
constexpr uint8_t kWhoAmIKnown[] = {0x11, 0x33, 0x32, 0x44};
} // namespace

bool Sc7a20h::WriteReg(uint8_t reg, uint8_t value) {
    if (!ready_ || !bus_valid_) {
        return false;
    }
    return power_i2c_write_reg(&bus_, addr_, reg, &value, 1, kI2cTimeoutMs) == ESP_OK;
}

bool Sc7a20h::ReadRegs(uint8_t reg, uint8_t* data, size_t len) {
    if (!ready_ || !bus_valid_ || data == nullptr || len == 0) {
        return false;
    }
    return power_i2c_write_read(&bus_, addr_, reg, data, len, kI2cTimeoutMs) == ESP_OK;
}

bool Sc7a20h::Configure() {
    if (!WriteReg(kRegCtrlReg1, kCtrlReg1Val)) {
        return false;
    }
    if (!WriteReg(kRegCtrlReg2, kCtrlReg2Val)) {
        return false;
    }
    if (!WriteReg(kRegCtrlReg3, kCtrlReg3Val)) {
        return false;
    }
    if (!WriteReg(kRegCtrlReg4, kCtrlReg4Val)) {
        return false;
    }
    if (!WriteReg(kRegCtrlReg5, kCtrlReg5Val)) {
        return false;
    }
    // INT1 开漏低有效：与 TCA 共用 GPIO2 时不把总线钳成推挽高
    if (!WriteReg(kRegCtrlReg6, kCtrlReg6Val)) {
        return false;
    }
    if (!WriteReg(kRegAoi1Ths, kAoi1ThsVal)) {
        return false;
    }
    if (!WriteReg(kRegAoi1Dur, kAoi1DurVal)) {
        return false;
    }
    return WriteReg(kRegAoi1Cfg, kAoi1CfgVal);
}

bool Sc7a20h::ReadAndClearAoi1(void) {
    uint8_t src = 0;
    if (!ReadRegs(kRegAoi1Src, &src, 1)) {
        return false;
    }
    return (src & kAoi1IaMask) != 0;
}

void Sc7a20h::WriteSilenceRegs(void) {
    WriteReg(kRegAoi1Cfg, 0x00);
    WriteReg(kRegCtrlReg3, 0x00);
    WriteReg(kRegCtrlReg5, 0x00);
    ReadAndClearAoi1();
    WriteReg(kRegCtrlReg1, 0x10); // ODR=1Hz、XYZ 关（勿 PD，易钳死共享 INT）
    WriteReg(kRegCtrlReg6, 0x00); // 推挽高有效：空闲驱高
}

void Sc7a20h::Deinit(void) {
    if (!ready_) {
        return;
    }
    WriteSilenceRegs();
    ready_ = false;
    ESP_LOGI(TAG, "deinit (INT1 PP high)");
}

bool Sc7a20h::SilenceSharedInt(const power_i2c_t* bus, uint8_t addr) {
    if (bus == nullptr) {
        return false;
    }
    bus_ = *bus;
    bus_valid_ = true;
    addr_ = addr;
    if (power_i2c_probe(&bus_, addr_, kProbeTimeoutMs) != ESP_OK) {
        return false;
    }
    ready_ = true; // 临时允许 WriteReg
    WriteSilenceRegs();
    ready_ = false;
    ESP_LOGI(TAG, "SilenceSharedInt @0x%02X ok", addr_);
    return true;
}

bool Sc7a20h::Begin(const power_i2c_t* bus, uint8_t addr) {
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
        ESP_LOGW(TAG, "SC7A20H @0x%02X probe failed: %s", addr_, esp_err_to_name(probe));
        return false;
    }

    ready_ = true;

    uint8_t who = 0;
    if (ReadRegs(kRegWhoAmI, &who, 1)) {
        last_who_am_i_ = who;
        bool known = false;
        for (uint8_t v : kWhoAmIKnown) {
            if (who == v) {
                known = true;
                break;
            }
        }
        if (known) {
            ESP_LOGI(TAG, "SC7A20H online @0x%02X WHO_AM_I=0x%02X", addr_, who);
        } else {
            ESP_LOGW(TAG, "SC7A20H @0x%02X WHO_AM_I=0x%02X（未知，best-effort）", addr_, who);
        }
    } else {
        ESP_LOGW(TAG, "SC7A20H ACK 通过但读 WHO_AM_I 失败");
    }

    if (!Configure()) {
        ESP_LOGW(TAG, "SC7A20H Configure 失败，保留 ready");
    }
    return true;
}

bool Sc7a20h::ReadAccelMg(int& ax, int& ay, int& az) {
    uint8_t buf[6] = {};
    if (!ReadRegs(static_cast<uint8_t>(kRegOutXL | kAutoIncMask), buf, sizeof(buf))) {
        return false;
    }
    const int16_t rx = static_cast<int16_t>((buf[1] << 8) | buf[0]);
    const int16_t ry = static_cast<int16_t>((buf[3] << 8) | buf[2]);
    const int16_t rz = static_cast<int16_t>((buf[5] << 8) | buf[4]);
    ax = static_cast<int>((rx >> 4) * kMgPerLsb);
    ay = static_cast<int>((ry >> 4) * kMgPerLsb);
    az = static_cast<int>((rz >> 4) * kMgPerLsb);
    return true;
}
