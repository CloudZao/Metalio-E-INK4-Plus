/*
 * 低功耗硬件操作（PA / CPU / 供电 / 引脚保持 / 唤醒）。
 * 仅硬件动作；场景组合由 PowerPolicy 决策后调用。由所选板级供电拓扑实现。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*prepare_network_power_off)(void);       // 共享供电切断前停止联网设备
    void (*network_power_restored)(bool recover);  // 恢复设备状态，可选异步联网
    void (*resume_network)(void);                  // 不改变供电状态，仅恢复联网
    bool (*sleep_cancelled)(void);                 // 业务尚未归还资源或存在唤醒请求
} PowerHardwareCallbacks;

/** @brief 启动时绑定系统协作回调，须早于板级构造及后台任务启动。 */
void power_hw_configure(const PowerHardwareCallbacks* callbacks);

/** @brief 通知系统在共享电源仍有效时停止联网设备。 */
void power_hw_network_prepare_off(void);

/** @brief 通知系统共享供电已恢复。 */
void power_hw_network_restored(bool recover);

/** @brief 请求恢复联网，不重新登记硬件上电时间。 */
void power_hw_network_resume(void);

/** @brief 查询系统是否取消本次入睡，未配置时禁止入睡。 */
bool power_hw_sleep_cancelled(void);

/** @brief 恢复关机绘制所需的本板硬件供电。 */
esp_err_t power_hw_prepare_power_off(void);

/** @brief 执行一次本板关机脉冲，调用方负责重试间隔。 */
esp_err_t power_hw_power_off_pulse(void);

/** @brief 初始化当前板型的浅睡依赖，启动时调用一次 */
esp_err_t power_hw_sleep_init(void);

/** @brief 外设初始化前记录复位状态并恢复本板启动所需的引脚保持。 */
void power_hw_boot_prepare(void);

/** @brief 在USB枚举后补发启动诊断，由策略节拍调用 */
void power_hw_report_sleep_reset(void);

/**
 * @brief 开关功放 PA
 * @param on true 打开，false 关闭
 * @return ESP_OK 成功
 */
esp_err_t power_hw_pa_set(bool on);

/** @brief 请求板级蓝牙电源复位；共享电源无法安全复位时返回不支持 */
esp_err_t power_hw_bt_power_reset(void);

/**
 * @brief 设置网络电源策略状态，由板级实现处理共享电源约束
 * @note 上电策略可请求蜂窝网络异步恢复，关闭不得误切共享设备供电。
 */
esp_err_t power_hw_main_rail_set(bool on);

/**
 * @brief MAIN_PWR 是否处于策略认定的上电态
 */
bool power_hw_main_rail_is_on(void);

/** @brief 查询共享电源状态是否已由板级确认 */
bool power_hw_main_rail_is_known(void);

/**
 * @brief CPU 定频（MHz），并关闭自动浅睡
 * @param mhz 目标频率；刷屏满频保护期间暂存，释放后应用最新请求
 * @return ESP_OK 成功；未开 CONFIG_PM_ENABLE 时返回 ESP_ERR_NOT_SUPPORTED
 */
esp_err_t power_hw_cpu_freq_set(int mhz);

/**
 * @brief 使用当前板型的唤醒输入阻塞浅睡一次
 * @param max_sleep_us 最长睡眠微秒；0 表示仅硬件输入唤醒
 * @note 成功返回时外围仍停放，退出待机或关机前须恢复。
 * @return ESP_OK 或唤醒相关错误
 */
esp_err_t power_hw_light_sleep_once(uint64_t max_sleep_us);

/** @brief SD访问排空后卸载并执行板级停放，重复调用无副作用 */
esp_err_t power_hw_sd_park(void);

/** @brief 恢复已休眠的 SD 和串口；日志在退出待机后恢复。 */
esp_err_t power_hw_sd_restore(void);

/** @brief 待机画面完成后停放板级外设与电源，可重试 */
esp_err_t power_hw_standby_rails_park(void);

/** @brief 恢复待机关闭的外设电源、通知驱动并恢复屏总线；未停放时不操作。 */
esp_err_t power_hw_standby_rails_restore(void);

/**
 * @brief 取最近一次浅睡返回瞬间的按键锁存
 * @param boot_low 非空则写入板级捕获的BOOT按下状态
 * @param power_low 非空则写入板级捕获的POWER按下状态
 */
void power_hw_gpio_wake_latch_get(bool* boot_low, bool* power_low);

/** @brief 领取实体按键唤醒锁存，位序与 InputKey 一致。 */
uint32_t power_hw_take_key_wake_latch(void);

#ifdef __cplusplus
}

/** @brief 扫描及恢复期间保持默认满频，离开作用域后恢复最新策略频率。 */
class PowerHwCpuFreqHold {
public:
    /** @brief 申请当前作用域的默认满频保持 */
    PowerHwCpuFreqHold();

    /** @brief 释放满频保持，恢复当前电源策略允许的频率 */
    ~PowerHwCpuFreqHold();
    PowerHwCpuFreqHold(const PowerHwCpuFreqHold&) = delete;
    PowerHwCpuFreqHold& operator=(const PowerHwCpuFreqHold&) = delete;
};
#endif
