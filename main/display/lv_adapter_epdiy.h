/**
 * @brief EPDiy + LVGL9 真刷：产品首页 / 百问钩子
 */
#pragma once

#include "display.h"

#include <chrono>
#include <esp_timer.h>

/**
 * @brief EPDiy + LVGL9：I1 真刷；转发百问 SetChatMessage / 状态栏
 */
class LvAdapterEpdiy : public Display {
public:
    /** @brief 当前适配器单例（Board Display 即本类） */
    static LvAdapterEpdiy* Instance();

    /** @brief LVGL 已初始化并可刷屏 / lv_async */
    bool IsReady() const {
        return lvgl_ready_;
    }

    /** @brief 抢 LVGL 递归锁（供 ScreenLvAsync 等非 DisplayLockGuard 路径） */
    bool LockUi(int timeout_ms) {
        return Lock(timeout_ms);
    }

    /** @brief 释放 LVGL 递归锁 */
    void UnlockUi() {
        Unlock();
    }

    /** @brief 初始化 LVGL、挂载资源/字库并加载首页 */
    bool Init();

    /** @brief 轮询 LVGL 定时器与 BOOT 键 */
    void Tick();

    /** @brief 标记系统准备完成 */
    void SetSystemReady();

    /** @brief 强制 GC16 全刷当前帧 */
    void FullRefresh();

    /** @brief 流式刷新期间抑制周期性 GC16（仅 DU） */
    void SetStreamPaintMode(bool on);

    /** @brief 状态栏标题前缀（如百问会话名）；空则清除 */
    void SetStatusTitlePrefix(const char* prefix);

    /**
     * @brief 从 resources mmap 取资源指针
     * @return true 找到
     */
    bool TryGetResource(const char* name, const uint8_t** mem, size_t* size) const;

    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void UpdateStatusBar(bool update_all = false) override;

    /** @brief 绑定当前页状态栏控件 */
    void BindStatusBar(lv_obj_t* status_label, lv_obj_t* notification_label,
                       lv_obj_t* network_label, lv_obj_t* battery_label,
                       lv_obj_t* battery_pct_label, lv_obj_t* mute_label);

    /**
     * @brief 兼容 397 BindStatusWidgets 参数序
     * @note network, mute, battery, status, notification, low_battery_popup[, battery_pct]
     */
    void BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery, lv_obj_t* status,
                           lv_obj_t* notification, lv_obj_t* /*low_battery_popup*/,
                           lv_obj_t* battery_pct = nullptr) {
        BindStatusBar(status, notification, network, battery, battery_pct, mute);
    }

    /** @brief 下一帧强制 GC16（仅打标，不同步 lv_refr_now，避免阅读首帧嵌套全刷打满 CPU） */
    void RequestFullOnNextCommit();

    /** @brief 下一帧强制 GC16 全刷（对齐 397 RequestNextFullRefresh） */
    void RequestNextFullRefresh() {
        RequestFullOnNextCommit();
        FullRefresh();
    }

    /** @brief EPDiy 无双平面；降级为下一帧强制 GC16（勿同步 refr） */
    void RequestDualPlanePartial() {
        RequestFullOnNextCommit();
    }

    /** @brief 进待机：只攒 FB 不上墨，随后 Park 一次全刷带出画面（对齐 397 defer） */
    void BeginStandbyEnterPaint();

    /** @brief 待机定格：一次 GC16 上屏后冻结 flush */
    void ParkEpdForStandby();

    /** @brief 退出待机：解冻；下一帧全刷（勿同步 GC16，以免堵 dismiss 日志） */
    void WakeEpdFromStandby();

    /** @brief 下一帧实际落墨后再恢复前光（配合退出待机） */
    void RequestRestoreFrontlightAfterCommit();

    /** @brief 待机期间暂停 touch_feed（对齐 397，避免 Park/GC16 误报触摸） */
    void PauseStandbyBackgroundWork();

    /** @brief 退出待机恢复 touch_feed */
    void ResumeStandbyBackgroundWork();

    /**
     * @brief 关机图：NVS 关机壁纸 → 内置 bg_shutdown.a2i1 → 「已关机」文案；刷完冻结 flush
     * @note 可在非 LVGL 任务调用（持内部锁）
     */
    void ShowPoweredOffScreen();

    /** @brief 关机前 Deep Sleep（S31：刷图后由 PWR_KEY 硬断，暂无独立 deep sleep） */
    void SleepEpdForPowerOff() {}

    /** @brief 触摸复位后踢 indev（S31 无 esp_lv_adapter 路径） */
    void KickTouchInput() {}

    /** @brief 冻结 / 解冻 LVGL→EPD flush */
    void FreezeFlush(bool on);

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    /** @brief 写 status 文案（含前缀）并缓存，供通知结束后恢复 */
    void ApplyStatusTextLocked(const char* status);
    /** @brief 换页重绑后立刻灌缓存（电量%/图标/静音/时钟），避免先空再刷 */
    void RestoreStatusWidgetsLocked();

    bool lvgl_ready_ = false;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* battery_pct_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    char status_title_prefix_[32] = {};
    char status_body_cache_[64] = {};
    char battery_pct_cache_[8] = {}; // 如「100%」；与 397 一致跨页保留
    int battery_level_cache_ = -1;
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    esp_timer_handle_t notification_timer_ = nullptr;
    std::chrono::system_clock::time_point last_status_update_time_{};
};
