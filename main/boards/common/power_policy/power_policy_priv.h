/*
 * power_policy 目录内部头：调度层与子模块共享状态与 API。
 * 仅 power_policy.cc / power_policy_standby_lp.cc 包含。
 */
#pragma once

#include "board.h"
#include "device_state.h"
#include "power_policy.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>

namespace pwr {

enum class Mode : uint8_t {
    Full = 0,   // 电话/BT：满频 + PA
    NetActive,  // 满频 + 保 WiFi（PA 另算）
    KeepNet,    // 保留档位（评估暂不选用）
    AppIdle,    // 关 PA + 关 WiFi + 空闲降频
    StandbyUi,  // 浅睡待机
};

inline constexpr int kIdleToStandbySecDefault = 3 * 60;         // 无操作 → 待机默认秒数
inline constexpr int kNetGraceSecDefault = 30;                  // 断网前暂留默认秒数
inline constexpr int kStandbyToShutdownSecDefault = 30 * 60;    // 浅睡累计 → 关机默认秒数
inline constexpr uint64_t kLightSleepSliceUs = 60ULL * 1000 * 1000; // 单次浅睡切片上限
inline constexpr int kStandbyUiSettleMs = 2000;                 // 进待机攒帧上限（就绪即提前 Park）
inline constexpr bool kStandbyLpUseLightSleep = true; // true=浅睡；false=vTaskDelay（串口排查）
/** 浅睡切片诊断：PSRAM 记 wake/awake，退出待机一次性 ESP_LOG（串口浅睡会掉） */
inline constexpr bool kStandbyLpSleepTrace = true;

extern Board* board;                         // 当前板实例
extern SemaphoreHandle_t mu;                 // 策略互斥锁
extern uint8_t need_ref[static_cast<int>(PowerNeed::kCount)]; // PowerNeed 重入计数
extern DeviceState device_state;             // 最近一次业务态
extern Mode mode;                            // 当前电源档位
extern bool wifi_stopped_by_policy;          // 本策略是否已 pause WiFi
extern bool wifi_stop_pending;               // 待锁外 Pause WiFi
extern bool wifi_start_pending;              // 待锁外 Resume WiFi
extern bool main_rail_off_by_policy;         // 本策略是否已请求 MAIN_PWR 掉电
extern bool main_rail_off_pending;           // 待锁外 MAIN_PWR off
extern bool main_rail_on_pending;            // 待锁外 MAIN_PWR on（含 BT 模式1）
extern bool standby_show_pending;            // 待锁外排队 StandbyScreen::Show
extern bool restore_provisioning_ui_pending; // 待锁外恢复配网/激活 UI
extern bool touch_asleep;                    // 触摸芯片是否已写深睡
extern int idle_cpu_mhz;                     // AppIdle 目标 CPU MHz
extern int queued_cpu_mhz;                   // 最近投递/应用的目标 MHz；0=未知
extern int idle_to_standby_sec;              // 无操作满此时长（秒）→ 待机页
extern int net_grace_sec;                    // 断网前暂留（秒）
extern int standby_to_shutdown_sec;          // 待机浅睡累计满此时长（秒）→ 关机；0=永不
extern int64_t last_user_activity_us;        // 最近用户活动时间戳
extern int64_t standby_elapsed_us;           // 已闭合的待机时长（清醒段+已完成浅睡段）
extern int64_t standby_awake_mark_us;        // 当前清醒段起点；0=不在清醒段
extern bool standby_ui_requested;            // 已请求显示待机 Overlay
extern bool shutdown_requested;              // 已发起硬关机
extern esp_timer_handle_t tick;              // 1s 评估定时器
extern TaskHandle_t lp_task;                 // 待机浅睡任务
extern bool lp_session_active;               // LP 会话进行中
extern bool standby_wake_exit;               // 本轮退出待机：挡请求位重进

/** @brief 无操作 → 待机阈值（微秒） */
inline int64_t UserIdleToStandbyUs()
{
    return static_cast<int64_t>(idle_to_standby_sec) * 1000000LL;
}

/** @brief 断网前暂留阈值（微秒） */
inline int64_t NetGraceUs()
{
    return static_cast<int64_t>(net_grace_sec) * 1000000LL;
}

/** @brief 清除断网暂留（兼容调用点；计时改由 last_user_activity_us 推导） */
void ClearNetGraceLocked();

/** @brief 浅睡累计 → 关机阈值（微秒） */
inline int64_t StandbyToShutdownUs()
{
    return static_cast<int64_t>(standby_to_shutdown_sec) * 1000000LL;
}

/** @brief 档位名（日志用） */
const char* ModeName(Mode m);

/** @brief 持锁重算并 Apply */
void ReevaluateLocked();
/** @brief 释放锁后投递/执行待改 CPU 频，并执行待 Pause/Resume WiFi */
void FlushCpuFreqOutsideLock();
/** @brief 持锁登记目标 CPU MHz（同值跳过） */
void QueueCpuMhzLocked(int mhz);
/** @brief 持锁发起硬关机（幂等） */
void BeginPowerOffLocked();

/** @brief 若未在会话中则唤醒 standby_lp 任务 */
void ArmStandbyLpTaskLocked();
/** @brief 异步 Show 待机 Overlay（幂等） */
void RequestStandbyUiAsync();
/** @brief 清待机累计与清醒 mark */
void ClearStandbyElapsedLocked();
/** @brief 待机计时归零并打开清醒段 */
void ResetStandbyElapsedLocked();
/** @brief 触摸在睡则硬件 RST 唤醒 */
void WakeTouchLocked();
/** @brief 触摸未睡则写 A5 深睡 */
void EnsureTouchSleepLocked();
/** @brief 待机累计是否已达关机阈值 */
bool StandbyUiLongEnoughLocked();
/** @brief 待机超时：打日志并 BeginPowerOff */
void RequestShutdownLocked();
/** @brief S31 无 iot_button；占位以保持与调度层 API 对称 */
void ResumeIotKeysIfNeeded();
/** @brief 标记退出待机并清累计（挡重进） */
void PrepareStandbyWakeExitLocked();
/** @brief 持锁准备退出并异步 Dismiss Overlay */
void RequestDismissStandbyOverlay();
/** @brief Init 时创建 standby_lp 任务（幂等） */
void StartStandbyLpTask();

}  // namespace pwr
