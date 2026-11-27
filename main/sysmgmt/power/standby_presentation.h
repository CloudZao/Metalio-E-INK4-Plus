#pragma once

#include <cstdint>

// 待机 Overlay：叠加在当前页面之上，不销毁底层页面。
class StandbyPresentation {
public:
    /** @brief 在当前 active screen 上叠加全屏待机页；须在 LVGL 线程调用 */
    static void Show();

    /** @brief 关闭待机 Overlay，并恢复进入前页面 */
    static void Dismiss();

    /** @brief Overlay 是否处于显示状态 */
    static bool IsActive();

    /** @brief 已退出待机但唤醒画面尚未物理完成；期间前台业务保持暂停 */
    static bool IsResuming();

    /** @brief 获取累计待机毫秒数，包含当前尚未结束的待机区间；须在界面上下文调用 */
    static uint64_t TotalDurationMs();

    /** @brief 保存唤醒请求，assistant 指定恢复后是否进入百问；请求保留至硬件恢复完成。 */
    static void RequestWake(bool assistant = false);

    /** @brief 查询是否存在尚未消费的唤醒请求。 */
    static bool HasWakeRequest();

    /** @brief 硬件及 SD 恢复后，在 LVGL 上下文消费唤醒请求并关闭待机页面。 */
    static void FinishWake();

    /** @brief 待机内容是否已就绪（经典页：至少完成一帧 flush；壁纸：解码就绪） */
    static bool IsPaintReady();

    /** @brief LVGL flush 末包完成时调用（供 epdiy flush_cb） */
    static void NotifyFlushCompleted();

    /** @brief 待机 UI 就绪后推白定格画面，成功返回 true */
    static bool EnterEpdSleep();

    /** @brief 退出待机前恢复 EPD flush；未冻结则为空操作 */
    static void ExitEpdSleep();

    /** @brief BOOT 短按：待机中忽略，不退出 Overlay */
    static bool HandleBootClick();

    /** @brief BOOT 长按：满时长后摘掉 Overlay 并打开百问 */
    static bool HandleBootLongPress();
};

/** @brief 关机失败后恢复输入及首页，界面任务调用。 */
void UiRollbackPowerOff();

/** @brief 准备待机底图并暂停校准，界面任务调用。 */
void UiPrepareStandbyOverlay();

/** @brief 在关闭存储准入前暂停应用活动，界面任务调用。 */
void UiPauseStandbyApplications();

/** @brief 存储准入关闭后暂停界面后台任务，界面任务调用。 */
void UiPauseStandbyViews();

/** @brief 恢复待机前的校准状态，界面任务调用。 */
void UiResumeStandbyCalibration();

/** @brief 恢复应用活动，界面任务调用。 */
void UiResumeStandbyApplications();

/** @brief 存储恢复后更新页面并恢复界面后台任务。 */
void UiResumeStandbyViews();

/** @brief 设置壁纸解码暂停状态，界面任务调用。 */
void UiPauseWallpaperDecode(bool paused);

/** @brief 完成唤醒后恢复壁纸及传输视图。 */
void UiFinishStandbyResume();

/** @brief 进入待机前停止传输视图。 */
void UiStopTransferForStandby();
