#ifndef BQ27220_GAUGE_H
#define BQ27220_GAUGE_H

#include "power_i2c.h"

#include <cstdint>

// BQ27220 电量计：单例封装，板级初始化后直接通过 GetInstance() 使用。
// Begin() 做地址 ACK；设备未挂上只返回 false。GetBatteryLevel() 用电压线性 SOC + 单向步进。
class Bq27220Gauge {
public:
    static constexpr uint8_t kDefaultAddr = 0x55;

    static Bq27220Gauge& GetInstance() {
        static Bq27220Gauge instance;
        return instance;
    }

    /**
     * @brief 挂到已有旧版 I2C 总线；重复调用且已就绪时直接返回 true
     * @note probe NACK 时返回 false；GetBatteryLevel 内会节流自愈重试
     */
    bool Begin(const power_i2c_t* bus, uint8_t addr = kDefaultAddr);

    /** @brief 是否已探测成功 */
    bool IsReady() const { return ready_; }

    bool ReadVoltageMv(uint16_t& mv);
    bool ReadCurrentMa(int16_t& current_ma);
    bool GetVoltageMv(uint16_t& mv) { return ReadVoltageMv(mv); }

    /**
     * @brief 电量与充放电状态（对齐 Board::GetBatteryLevel）
     * @param level 0..100；≥4.33V 连续 30s 才允许 100%
     * @param charging 电流 > +5mA 且 level < 100
     * @param discharging 电流 < -5mA
     */
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging);

    /** @brief 重置滑动平均与步进显示 */
    void ResetFilter();

private:
    Bq27220Gauge() = default;
    Bq27220Gauge(const Bq27220Gauge&) = delete;
    Bq27220Gauge& operator=(const Bq27220Gauge&) = delete;

    bool ReadU16(uint8_t reg, uint16_t* out);
    float FilterPush(float sample);
    int ApplyMonoStep(int target_pct, bool charging);

    static constexpr int kFilterSize = 60;
    static constexpr int64_t kSocStepUs = 30LL * 1000 * 1000;
    static constexpr int64_t kFullHoldUs = 30LL * 1000 * 1000;
    static constexpr uint16_t kFullConfirmMv = 4330;
    static constexpr uint16_t kFullExitMv = 4280;

    power_i2c_t bus_{};
    bool        bus_valid_ = false;
    uint8_t     addr_ = kDefaultAddr;
    bool        ready_ = false;

    int consecutive_err_ = 0;
    int retry_counter_ = 0;

    float filter_buf_[kFilterSize] = {0};
    int   filter_idx_ = 0;
    int   filter_count_ = 0;
    float filter_sum_ = 0.0f;
    bool  filter_primed_ = false;

    int     displayed_soc_ = -1;
    int64_t last_soc_step_us_ = 0;
    int64_t full_above_since_us_ = 0;
    int     last_charge_dir_ = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

/** @brief CX25601N VREG 任务用：电池电压 mV */
signed int battery_get_bat_voltage(void);
/** @brief CX25601N VREG 任务用：电流 0.1µA 标度（1mA→10000） */
signed int battery_get_bat_current(void);

#ifdef __cplusplus
}
#endif

#endif // BQ27220_GAUGE_H
