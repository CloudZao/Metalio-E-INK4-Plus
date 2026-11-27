/*
 * 低功耗策略：活动位 + 用户无操作计时 → 单点评估 → power_hw 原子执行。
 * 业务直接 PowerPolicy::GetInstance()；勿经 Board 转发。
 *
 * 档位摘要：Full=电话/BT；NetActive=满频保网（PA 另算）；
 * AppIdle=关 WiFi + 空闲降频（频率由所选板型配置提供）；断网前可暂留 NetActive（30/60/120s 或永不）。
 * PA：Full（电话/BT）、Speaking、音频测试或 LocalPlayback；Connecting/Listening 保网但不拉 PA。
 * 翻译页 UiKeepNet 不参与 PA_en。
 * StandbyInhibit / UsbVirtualDisk：禁无操作进待机，不硬占网、不拉 PA。
 */
#pragma once

#include <cstdint>
#include "config.h"
#include "sysmgmt/device_state.h"
#include "boards/common/input/input.h"

class Board;

static_assert(BOARD_IDLE_TO_STANDBY_SECONDS > 0, "无操作待机时长须大于零");

inline constexpr int kPowerPolicyNetGraceOptionsSec[] = BOARD_NET_GRACE_OPTIONS_SECONDS;
inline constexpr int kPowerPolicyNetGraceSecDefault = BOARD_NET_GRACE_SECONDS;
inline constexpr int kPowerPolicyStandbyOffOptionsSec[] = BOARD_STANDBY_OFF_OPTIONS_SECONDS;
inline constexpr int kPowerPolicyStandbyOffSecDefault = BOARD_STANDBY_OFF_SECONDS;

static_assert([] {
    bool has_default = false;
    for (int sec : kPowerPolicyNetGraceOptionsSec) {
        if (sec < 0) {
            return false;
        }
        has_default |= sec == kPowerPolicyNetGraceSecDefault;
    }
    return has_default;
}(), "空闲保网档位须非负且包含默认值");

static_assert([] {
    bool has_default = false;
    for (int sec : kPowerPolicyStandbyOffOptionsSec) {
        if (sec <= 0) {
            return false;
        }
        has_default |= sec == kPowerPolicyStandbyOffSecDefault;
    }
    return has_default;
}(), "自动关机档位须为正数且包含默认值");

/** 业务向电源策略声明的活动需求（可叠加） */
enum class PowerNeed : uint8_t {
    PhoneCall = 0,      // 蜂窝通话：满功率 + PA
    BtAudio,            // BT 通话/音频：满功率 + PA
    AudioSession,       // 百问云端通道：待命 NetActive；播报靠 DeviceState 开 PA
    LocalMic,           // 本地录音/实时翻译采麦：硬占网，不驱动 PA
    OtaDownload,        // OTA/短时 HTTP/云同步：硬占网；持有期间不计无操作进待机
    UiKeepNet,          // 翻译页保网满频；本页不参与 PA_en
    UsbVirtualDisk,     // 虚拟 U 盘启用：不计无操作进待机（不硬占网）
    StandbyInhibit,     // 产测老化等：禁止无操作进待机；不硬占网、不驱动 PA（PA 仍靠 DeviceState）
    PeripheralReset,    // 共享电源复位：保持活动、保网，强制关闭 PA
    ForegroundNetwork,  // 前台联网页面及其查询：软保网，不刷新无操作计时，不干预 PA
    AudioTesting,       // 录音及老化测试：保持性能与PA，不覆盖整机和会话状态
    LocalPlayback,      // 本地音频播放：保持功放，不要求联网
    kCount
};

class PowerPolicy {
public:
    /**
     * @brief 获取电源策略单例
     */
    static PowerPolicy& GetInstance();

    /**
     * @brief 板构造末尾注册 Board，并启动评估定时器
     * @param board 当前板实例
     */
    void Init(Board* board);

    /**
     * @brief 声明一项活动需求（可重入计数）
     * @param need 需求类型
     */
    void Acquire(PowerNeed need);

    /**
     * @brief 释放一项活动需求
     * @param need 需求类型
     */
    void Release(PowerNeed need);

    /** @brief 前台联网页面或其请求是否尚未归还引用，供待机任务等待收尾 */
    bool HasForegroundNetworkWork() const;

    /** @brief 查询尚未完成的联网、射频及共享电源动作，供待机排空。 */
    bool HasPendingHardwareWork() const;

    /**
     * @brief 同步设备业务态（聆听/升级等驱动硬占网）
     * @param state 当前 DeviceState
     */
    void NotifyDeviceState(DeviceState state);

    /**
     * @brief 触摸/按键：重置无操作计时；待机 settle 期内点屏则取消浅睡
     */
    void NotifyUserActivity();

    /** 触摸芯片是否已深睡（待机 LP）；touch_feed 须跳过 I2C 读以免刷屏报错 */
    bool IsTouchAsleep() const;

    /**
     * @brief 请求立即重算电源档位
     */
    void RequestReevaluate();

    /**
     * @brief 待机 Overlay 已关闭：清待机请求并重算
     */
    void OnStandbyOverlayDismissed();

    /**
     * @brief POWER 长按或待机超时：走硬关机（内部调 power_hw）
     */
    void RequestPowerOff();

    /** @brief 关机准备失败后恢复触摸和策略状态，允许用户重试。 */
    void OnPowerOffFailed();

    /**
     * @brief 读取 AppIdle 目标 CPU 频率（MHz）
     * @return 所选板型支持的频率，单位MHz
     */
    int GetIdleCpuMhz() const;

    /**
     * @brief 设置 AppIdle 目标 CPU 频率并写入 NVS
     * @param mhz 板型支持的频率，其它值归一为板级默认值
     */
    void SetIdleCpuMhz(int mhz);

    /**
     * @brief 无保网需求后仍保持联网的时长（秒）
     * @return 板级配置中的秒数，0表示永不因空闲断网
     */
    int GetNetGraceSec() const;

    /**
     * @brief 设置断网前暂留时长并写入 NVS
     * @param sec 仅接受板级配置的档位，其它值归一为板级默认值；0不影响待机停网
     */
    void SetNetGraceSec(int sec);

    /** @brief 获取待机累计自动关机时长，单位秒 */
    int GetStandbyToShutdownSec() const;

    /** @brief 设置自动关机时长并异步保存，接受板级配置的档位，其他值恢复板级默认值 */
    void SetStandbyToShutdownSec(int sec);

    /**
     * @brief 电源键短按进待机：跳过断网暂留
     */
    void PreparePowerKeyStandby();

private:
    PowerPolicy() = default;
    PowerPolicy(const PowerPolicy&) = delete;
    PowerPolicy& operator=(const PowerPolicy&) = delete;
};

/**
 * @brief RAII 持有 PowerNeed；须在 vTaskDelete 前离开作用域以 Release
 */
class PowerNeedHold {
public:
    explicit PowerNeedHold(PowerNeed need) : need_(need) {
        PowerPolicy::GetInstance().Acquire(need_);
    }
    ~PowerNeedHold() {
        PowerPolicy::GetInstance().Release(need_);
    }
    PowerNeedHold(const PowerNeedHold&) = delete;
    PowerNeedHold& operator=(const PowerNeedHold&) = delete;

private:
    PowerNeed need_;
};
