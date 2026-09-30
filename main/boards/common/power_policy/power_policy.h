/*
 * 低功耗策略：活动位 + 用户无操作计时 → 单点评估 → power_hw 原子执行。
 * 业务直接 PowerPolicy::GetInstance()；勿经 Board 转发。
 *
 * 档位摘要：Full=电话/BT；NetActive=满频保网（PA 另算）；
 * AppIdle=关 WiFi + 空闲降频（设置可选 160/240/320，默认 320）；断网前可暂留 NetActive（30/60/120s）。
 * PA：仅 Full（电话/BT）或 Speaking/音频测试；Connecting/Listening 保网但不拉 PA。
 * 翻译页 UiKeepNet 不参与 PA_en。
 * StandbyInhibit / UsbVirtualDisk：禁无操作进待机，不硬占网、不拉 PA。
 */
#pragma once

#include "device_state.h"

#include <cstdint>

class Board;

/** BOOT 长按阈值（ms）：百问 PTT / 首页进页（待机 Overlay 不响应 BOOT） */
inline constexpr uint16_t kBootLongPressMs = 500;
/** POWER 长按阈值（ms）：硬关机；待机浅睡量时长同此 */
inline constexpr uint16_t kPowerLongPressMs = 3000;

/** 业务向电源策略声明的活动需求（可叠加） */
enum class PowerNeed : uint8_t {
    PhoneCall = 0, // 蜂窝通话：满功率 + PA
    BtAudio,       // BT 通话/音频：满功率 + PA
    AudioSession,  // 百问云端通道：待命 NetActive；播报靠 DeviceState 开 PA
    LocalMic,      // 本地录音/实时翻译采麦：硬占网，不驱动 PA
    OtaDownload,   // OTA/短时 HTTP/云同步：硬占网；持有期间不计无操作进待机
    UiKeepNet,     // 翻译页保网满频；本页不参与 PA_en
    UsbVirtualDisk, // 虚拟 U 盘启用：不计无操作进待机（不硬占网）
    StandbyInhibit, // 产测老化等：禁止无操作进待机；不硬占网、不驱动 PA（PA 仍靠 DeviceState）
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

    /**
     * @brief 读取 AppIdle 目标 CPU 频率（MHz）
     * @return 160 / 240 / 320
     */
    int GetIdleCpuMhz() const;

    /**
     * @brief 设置 AppIdle 目标 CPU 频率并写入 NVS
     * @param mhz 仅接受 160 / 240 / 320，其它值归一为默认 320
     */
    void SetIdleCpuMhz(int mhz);

    /**
     * @brief 无操作多久进入浅睡待机（秒）
     * @return 180 / 600 / 1800
     */
    int GetUserIdleToStandbySec() const;

    /**
     * @brief 设置无操作进浅睡待机时长并写入 NVS
     * @param sec 仅接受 180 / 600 / 1800，其它值归一为默认 180
     */
    void SetUserIdleToStandbySec(int sec);

    /**
     * @brief 无保网需求后仍保持联网的时长（秒）
     * @return 30 / 60 / 120
     */
    int GetNetGraceSec() const;

    /**
     * @brief 设置断网前暂留时长并写入 NVS
     * @param sec 仅接受 30 / 60 / 120，其它值归一为默认 30
     */
    void SetNetGraceSec(int sec);

    /**
     * @brief 电源键短按进待机：跳过断网暂留
     */
    void PreparePowerKeyStandby();

    /**
     * @brief 待机浅睡累计多久自动关机（秒）
     * @return 0（永不）/ 180 / 600 / 1800 / 10800 / 21600
     */
    int GetStandbyToShutdownSec() const;

    /**
     * @brief 设置浅睡累计关机时长并写入 NVS
     * @param sec 仅接受 0 / 180 / 600 / 1800 / 10800 / 21600，其它值归一为默认 1800；0=永不关机（浅睡仍按 60s 切片定时醒）
     */
    void SetStandbyToShutdownSec(int sec);

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
    explicit PowerNeedHold(PowerNeed need) : need_(need)
    {
        PowerPolicy::GetInstance().Acquire(need_);
    }
    ~PowerNeedHold()
    {
        PowerPolicy::GetInstance().Release(need_);
    }
    PowerNeedHold(const PowerNeedHold&) = delete;
    PowerNeedHold& operator=(const PowerNeedHold&) = delete;

private:
    PowerNeed need_;
};
