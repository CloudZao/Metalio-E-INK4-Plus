/*
 * 低功耗硬件原子（PA / WiFi / CPU / 浅睡 / 硬关机）。
 * 仅硬件动作；场景组合由 PowerPolicy 决策后调用。Metalio 板实现。
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 S31 浅睡校准依赖；启动时调用一次，保留看门狗保护。 */
esp_err_t power_hw_sleep_init(void);
/** Log reset reason and retained sleep checkpoint before peripheral initialization. */
void power_hw_log_sleep_reset(void);
/** Replay the previous-boot diagnostic after USB enumeration; call from policy tick. */
void power_hw_report_sleep_reset(void);

/** 退出实际待机后按需恢复 4G；定时唤醒只恢复电源，不启动搜网。 */
void power_hw_cellular_resume(void);

/**
 * @brief 开关功放 PA
 * @param on true 打开，false 关闭
 * @return ESP_OK 成功
 */
esp_err_t power_hw_pa_set(bool on);
/** @brief 蓝牙共享 P14 电源低 3 秒后高；调用方须保持唤醒。 */
esp_err_t power_hw_bt_power_reset(void);

/**
 * @brief 网络电源策略标记；S31 实际 P14 在浅睡路径关闭/恢复。
 * @note 上电策略可请求 4G 异步恢复；false 不直接切 P14（与蓝牙/功放共享）。
 */
esp_err_t power_hw_main_rail_set(bool on);

/**
 * @brief MAIN_PWR 是否处于策略认定的上电态
 */
bool power_hw_main_rail_is_on(void);

/**
 * @brief 待机浅睡：暂停射频（保留 netif）
 * @return ESP_OK 成功
 */
esp_err_t power_hw_wifi_stop(void);

/**
 * @brief 退出待机：恢复射频
 * @return ESP_OK 成功
 */
esp_err_t power_hw_wifi_start(void);

/**
 * @brief CPU 定频（MHz），并关闭自动浅睡
 * @param mhz 目标频率；刷屏满频保护期间暂存，释放后应用最新请求
 * @return ESP_OK 成功；未开 CONFIG_PM_ENABLE 时返回 ESP_ERR_NOT_SUPPORTED
 */
esp_err_t power_hw_cpu_freq_set(int mhz);

/**
 * @brief 启动硬关机任务（关 PA/保轨 → 关机图 → Deep Sleep → settle → 屏脚 park → PWR_KEY）
 * @note 幂等；POWER 长按与策略超时共用；内部 hold MAIN_PWR 且不重发 BT
 */
void power_hw_begin_power_off(void);

/**
 * @brief 阻塞浅睡一次；仅 P17→GPIO2(EXT1) 唤醒，max_sleep_us>0 时兼定时唤醒
 * @param max_sleep_us 最长睡眠微秒；0 表示仅 EXT1 唤醒
 * @note 入睡前 mask TCA 多余 INT 源、松加速度计 INT；仅 EXT1(GPIO2) 唤醒；醒后可 latch_get
 * @return ESP_OK 或唤醒相关错误
 */
esp_err_t power_hw_light_sleep_once(uint64_t max_sleep_us);

/** @brief SD 访问排空后卸载并锁存引脚低电平；重复调用无副作用。 */
esp_err_t power_hw_sd_park(void);
/** @brief 恢复已休眠的 SD 和串口；日志在退出待机后恢复。 */
esp_err_t power_hw_sd_restore(void);

/**
 * @brief 取最近一次浅睡返回瞬间的按键锁存
 * @param boot_low 非空则写 false（保留形参兼容 397）
 * @param power_low 非空则写醒瞬间 P17 是否为低（按下）
 */
void power_hw_gpio_wake_latch_get(bool* boot_low, bool* power_low);

#ifdef __cplusplus
}

/** @brief 扫描及恢复期间保持默认满频，离开作用域后恢复最新策略频率。 */
class PowerHwCpuFreqHold {
public:
    PowerHwCpuFreqHold();
    ~PowerHwCpuFreqHold();
    PowerHwCpuFreqHold(const PowerHwCpuFreqHold&) = delete;
    PowerHwCpuFreqHold& operator=(const PowerHwCpuFreqHold&) = delete;
};
#endif
