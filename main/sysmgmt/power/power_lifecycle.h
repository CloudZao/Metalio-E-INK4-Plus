#pragma once

#include <esp_err.h>

/** @brief 在板级初始化前绑定电源生命周期协作入口。 */
void PowerLifecycleInit();

/** @brief 实际退出待机后恢复蜂窝联网，定时唤醒不调用。 */
void PowerLifecycleResumeCellular();

/** @brief 暂停当前联网射频，保留网络对象。 */
esp_err_t PowerLifecyclePauseNetwork();

/** @brief 按用户启用状态和当前联网方式恢复连接。 */
esp_err_t PowerLifecycleResumeNetwork();

/** @brief 请求关机协调任务，分配失败返回false，重复请求返回true。 */
bool PowerLifecycleBeginPowerOff();

/** @brief 读取系统待机状态，可从后台任务调用 */
bool PowerLifecycleIsStandby();

/** @brief 在待机界面提交或退出时发布系统状态 */
void PowerLifecycleSetStandby(bool standby);

/** @brief 启动低电保护关机协作，失败时返回错误且不阻断芯片保护 */
esp_err_t PowerLifecycleBeginBatteryShutdown(bool low_voltage);

/** @brief 在创建待机覆盖层前暂停校准并准备底图，UI 线程调用 */
void PowerLifecyclePrepareStandbyOverlay();

/** @brief 待机覆盖层提交后暂停各功能并关闭存储准入，UI 线程调用 */
void PowerLifecyclePauseStandbyActivities();

/** @brief 唤醒后先恢复存储准入及日志生产 */
void PowerLifecycleResumeStandbyStorage();

/** @brief 待机覆盖层卸载后恢复业务和显示后台任务，UI 线程调用 */
void PowerLifecycleResumeStandbyActivities();

/** @brief 完成界面恢复后重新开放壁纸解码及传输展示 */
void PowerLifecycleFinishStandbyResume();

/** @brief 记录待机唤醒首帧窗口，pending为false时取消该窗口。 */
void PowerLifecycleSetStandbyResuming(bool pending);

/** @brief 查询唤醒首帧是否尚未落墨，刷新失败时也会结束窗口。 */
bool PowerLifecycleIsStandbyResuming();

/** @brief 查询是否已经进入关机协调，供后台外设任务停止采集 */
bool PowerLifecycleIsPoweringOff();

/**
 * @brief 查询待机尚未归还的存储、启动、音频、网络或硬件工作；空指针表示此刻已排空。
 * @note 暂停事件仅发出请求。所有者须在真正退出前保留SD事务或资源占用；硬件切换仍须重新校验。
 */
const char* PowerLifecyclePendingStandbyWork();
