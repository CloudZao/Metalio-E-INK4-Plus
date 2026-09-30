#ifndef SC7A20H_H
#define SC7A20H_H

#include "power_i2c.h"

#include <cstddef>
#include <cstdint>

/**
 * @brief SC7A20H 三轴加速度计（寄存器兼容 LIS2DH12）
 * @note 默认地址 0x19（SDO 悬空/高）；SDO 接地为 0x18。板级可先试 0x19 再试 0x18。
 */
class Sc7a20h {
public:
    static constexpr uint8_t kDefaultAddr = 0x19;
    static constexpr uint8_t kAltAddr = 0x18;

    static Sc7a20h& GetInstance() {
        static Sc7a20h instance;
        return instance;
    }

    /**
     * @brief 挂到旧版 I2C；probe 失败返回 false
     */
    bool Begin(const power_i2c_t* bus, uint8_t addr = kDefaultAddr);
    bool IsReady() const { return ready_; }

    uint8_t Addr() const { return addr_; }
    uint8_t LastWhoAmI() const { return last_who_am_i_; }

    /** @brief 加速度，单位 mg */
    bool ReadAccelMg(int& ax, int& ay, int& az);

    /**
     * @brief 读并清除 AOI1 锁存（INT1 运动中断源）
     * @return true 表示本次中断由 AOI1 产生（IA 置位）
     */
    bool ReadAndClearAoi1(void);

    /**
     * @brief 关 INT1（推挽拉高松开线与）；不清总线对象
     */
    void Deinit(void);

    /**
     * @brief 未 Begin 时：探测并关 INT1/AOI（不跑 Configure）
     * @return true 探测并写寄存器成功
     */
    bool SilenceSharedInt(const power_i2c_t* bus, uint8_t addr);

private:
    Sc7a20h() = default;
    Sc7a20h(const Sc7a20h&) = delete;
    Sc7a20h& operator=(const Sc7a20h&) = delete;

    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadRegs(uint8_t reg, uint8_t* data, size_t len);
    bool Configure();
    void WriteSilenceRegs(void); // AOI/INT1 关，INT1 推挽高

    power_i2c_t bus_{};
    bool        bus_valid_ = false;
    bool        ready_ = false;
    uint8_t     addr_ = kDefaultAddr;
    uint8_t     last_who_am_i_ = 0;
};

#endif // SC7A20H_H
