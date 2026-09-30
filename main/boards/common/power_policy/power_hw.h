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

/**
 * @brief 开关功放 PA
 * @param on true 打开，false 关闭
 * @return ESP_OK 成功
 */
esp_err_t power_hw_pa_set(bool on);

/**
 * @brief 外设总电源轨 MAIN_PWR（与保网停/启同生命周期）
 * @param on true 上电；false 掉电（AppIdle/待机 Overlay 后）
 * @note 幂等；false→true 时 settle 后重发蓝牙模式1，并在 4G 模式下异步重建 NT26。
 *       掉电前会先 Stop 4G 模组。勿用 ESP_ERROR_CHECK 中断策略任务。
 * @return ESP_OK / ESP_ERR_INVALID_STATE（IO 未就绪）
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
 * @param mhz 目标频率；策略常用 80 或 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
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

/**
 * @brief 取最近一次浅睡返回瞬间的按键锁存
 * @param boot_low 非空则写 false（保留形参兼容 397）
 * @param power_low 非空则写醒瞬间 P17 是否为低（按下）
 */
void power_hw_gpio_wake_latch_get(bool* boot_low, bool* power_low);

#ifdef __cplusplus
}
#endif
