#ifndef PCF8563_H
#define PCF8563_H

#include "power_i2c.h"

#include <cstddef>
#include <ctime>
#include <cstdint>

/**
 * @brief NXP PCF8563 / BM8563 RTC 单例，地址 0x51
 * @note 板级 Begin(bus) 后：ApplyRtcToSystem / SyncSystemToRtc 做系统时间双向同步
 */
class Pcf8563 {
public:
    static constexpr uint8_t kDefaultAddr = 0x51;

    static Pcf8563& GetInstance() {
        static Pcf8563 instance;
        return instance;
    }

    /**
     * @brief 挂到已有旧版 I2C 总线；probe 失败返回 false
     */
    bool Begin(const power_i2c_t* bus, uint8_t addr = kDefaultAddr);
    bool IsReady() const { return ready_; }

    /**
     * @brief 读日历
     * @param valid 非空时：false 表示 VL 置位（时间可能不可信）
     */
    bool GetTime(struct tm& out, bool* valid = nullptr);
    bool SetTime(const struct tm& in);

    /** @brief 芯片时间 → settimeofday */
    bool ApplyRtcToSystem();
    /** @brief localtime(now) → 芯片 */
    bool SyncSystemToRtc();

    /**
     * @brief 清 Control_status_2 的 AF/TF（闹钟/定时中断标志）
     * @note INT 为开漏低有效；板级经 MOSFET 反相后 GPIO4 上升沿表示事件
     */
    bool ClearIrqFlags();

private:
    Pcf8563() = default;
    Pcf8563(const Pcf8563&) = delete;
    Pcf8563& operator=(const Pcf8563&) = delete;

    bool WriteRegs(uint8_t reg, const uint8_t* data, size_t len);
    bool ReadRegs(uint8_t reg, uint8_t* data, size_t len);

    static uint8_t DecToBcd(uint8_t v);
    static uint8_t BcdToDec(uint8_t v);

    power_i2c_t bus_{};
    bool        bus_valid_ = false;
    bool        ready_ = false;
    uint8_t     addr_ = kDefaultAddr;
};

#endif // PCF8563_H
