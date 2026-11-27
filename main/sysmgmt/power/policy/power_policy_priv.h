/*
 * power_policy 目录内部头：调度层与子模块共享状态与 API。
 * 仅 power_policy.cc / power_policy_standby_lp.cc 包含。
 */
#pragma once

#include <cstdint>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "boards/common/board/board.h"
#include "power_policy.h"
#include "sysmgmt/device_state.h"

enum class PowerPolicyMode : uint8_t {
    Full = 0,   // 电话/BT：满频 + PA
    NetActive,  // 满频 + 保 WiFi（PA 另算）
    KeepNet,    // 保留档位（评估暂不选用）
    AppIdle,    // 关 PA + 关 WiFi + 空闲降频
    StandbyUi,  // 浅睡待机
};

inline constexpr uint64_t kPowerPolicyLightSleepSliceUs = 60ULL * 1000 * 1000;  // 单次浅睡切片上限
inline constexpr bool kPowerPolicyStandbyLpUseLightSleep = true;  // true=浅睡；false=vTaskDelay（串口排查）

extern Board* power_policy_board;                                           // 当前板实例
extern SemaphoreHandle_t power_policy_mu;                                   // 策略互斥锁
extern uint8_t power_policy_need_ref[static_cast<int>(PowerNeed::kCount)];  // PowerNeed 重入计数
extern DeviceState power_policy_device_state;                               // 最近一次业务态
extern PowerPolicyMode power_policy_mode;                                   // 当前电源档位
extern bool power_policy_wifi_stopped_by_policy;                            // 本策略是否已 pause WiFi
extern bool power_policy_wifi_stop_pending;                                 // 待锁外 Pause WiFi
extern bool power_policy_wifi_start_pending;                                // 待锁外 Resume WiFi
extern bool power_policy_main_rail_off_by_policy;                           // 本策略是否已请求 MAIN_PWR 掉电
extern bool power_policy_main_rail_off_pending;                             // 待锁外 MAIN_PWR off
extern bool power_policy_main_rail_on_pending;                              // 待锁外 MAIN_PWR on（含 BT 模式1）
extern bool power_policy_standby_show_pending;                              // 待锁外排队 StandbyPresentation::Show
extern bool power_policy_restore_provisioning_ui_pending;                   // 待锁外恢复配网/激活 UI
extern bool power_policy_touch_asleep;                                      // 触摸芯片是否已写深睡
extern int power_policy_idle_cpu_mhz;                                       // AppIdle 目标 CPU MHz
extern int power_policy_queued_cpu_mhz;                                     // 最近投递/应用的目标 MHz；0=未知
extern int power_policy_net_grace_sec;                                      // 断网前暂留（秒）；0=永不因空闲断网
extern int power_policy_standby_to_shutdown_sec;                            // 待机累计自动关机时长，秒
extern int64_t power_policy_last_user_activity_us;                          // 最近用户活动时间戳
extern int64_t power_policy_standby_elapsed_us;                             // 已闭合的待机时长（清醒段+已完成浅睡段）
extern int64_t power_policy_standby_awake_mark_us;                          // 当前清醒段起点；0=不在清醒段
extern bool power_policy_standby_ui_requested;                              // 已请求显示待机 Overlay
extern bool power_policy_shutdown_requested;                                // 已发起硬关机
extern esp_timer_handle_t power_policy_tick;                                // 1s 评估定时器
extern TaskHandle_t power_policy_lp_task;                                   // 待机浅睡任务
extern bool power_policy_lp_session_active;                                 // LP 会话进行中
extern bool power_policy_standby_wake_exit;                                 // 本轮退出待机：挡请求位重进

/** @brief 无操作 → 待机阈值（微秒） */
inline int64_t PowerPolicyUserIdleToStandbyUs() {
    return static_cast<int64_t>(BOARD_IDLE_TO_STANDBY_SECONDS) * 1000000LL;
}

/** @brief 断网前暂留阈值（微秒） */
inline int64_t PowerPolicyNetGraceUs() {
    return static_cast<int64_t>(power_policy_net_grace_sec) * 1000000LL;
}

/** @brief 浅睡累计 → 关机阈值（微秒） */
inline int64_t PowerPolicyStandbyToShutdownUs() {
    return static_cast<int64_t>(power_policy_standby_to_shutdown_sec) * 1000000LL;
}

/** @brief 档位名（日志用） */
const char* PowerPolicyModeName(PowerPolicyMode m);

/** @brief 持锁重算并 Apply */
void PowerPolicyReevaluateLocked();

/** @brief 释放锁后投递/执行待改 CPU 频，并执行待 Pause/Resume WiFi */
void PowerPolicyFlushCpuFreqOutsideLock();


/** @brief 持锁登记目标 CPU MHz（同值跳过） */
void PowerPolicyQueueCpuMhzLocked(int mhz);

/** @brief 持锁发起硬关机（幂等） */
void PowerPolicyBeginPowerOffLocked();

/** @brief 若未在会话中则唤醒 standby_lp 任务 */
void PowerPolicyArmStandbyLpTaskLocked();

/** @brief 异步 Show 待机 Overlay（幂等） */
void PowerPolicyRequestStandbyUiAsync();

/** @brief 清待机累计与清醒 mark */
void PowerPolicyClearStandbyElapsedLocked();

/** @brief 待机计时归零并打开清醒段 */
void PowerPolicyResetStandbyElapsedLocked();

/** @brief 触摸在睡则硬件 RST 唤醒 */
void PowerPolicyWakeTouchLocked();

/** @brief 触摸未睡则写 A5 深睡 */
void PowerPolicyEnsureTouchSleepLocked();

/** @brief 待机累计是否已达关机阈值 */
bool PowerPolicyStandbyUiLongEnoughLocked();

/** @brief 待机超时：打日志并 BeginPowerOff */
void PowerPolicyRequestShutdownLocked();

/** @brief 标记退出待机并清累计（挡重进） */
void PowerPolicyPrepareStandbyWakeExitLocked();

/** @brief 持锁准备退出并异步 Dismiss Overlay */
void PowerPolicyRequestDismissStandbyOverlay();

/** @brief Init 时创建 standby_lp 任务（幂等） */
void PowerPolicyStartStandbyLpTask();
