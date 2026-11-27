#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_timer.h>
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "ui/common/drawing/display.h"
#include "ui/common/widgets/screen_common.h"

struct EpdFlushCtx;
struct EpdStatusBar;

struct TouchVirtualKey {
    const char* name;
    int16_t x;
    int16_t y;
    // 0=按下即触发 Click；>0=延迟到松手再触发，并在阈值到达时发 LongPress。
    uint16_t long_press_ms = 0;
};

enum class TouchVkEvent : uint8_t {
    Click = 0,      // 无长按配置的即时短按
    LongPress = 1,  // 达到 long_press_ms（仅配置了长按的键）
    PressUp = 2,    // 松开（仅配置了长按的键）
    PressDown = 3,  // 按下（仅配置了长按的键）
};

using TouchVirtualKeyEventCb = bool (*)(const char* name, TouchVkEvent event, void* user_data);

class LvAdapterSsd1677 : public Display {
public:
    /** @brief 绑定面板、触摸及屏幕尺寸，初始化显示刷新和输入处理。 */
    LvAdapterSsd1677(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     esp_lcd_touch_handle_t touch_handle, int width, int height);

    /** @brief 停止触摸采集和通知定时器，排空刷新并释放适配器缓存。 */
    ~LvAdapterSsd1677() override;

    /** @brief 请求当前画面完整刷新 */
    void FullRefresh();

    /** @brief 设置流式绘制状态，SPI后端仍使用自身刷新策略 */
    void SetStreamPaintMode(bool on);

    /** @brief 在后台调用方显示低电量关机提示并等待物理完成 */
    bool ShowLowBatteryShutdownNotice(bool low_voltage);

    /** @brief 准备待机叠图，SPI后端在待机提交时读取底图 */
    void PrepareWallpaperOverlay();

    /** @brief 无前光板无需延后恢复前光 */
    void RequestRestoreFrontlightAfterCommit();

    /** @brief 获取当前适配器实例；板级构造期间用此入口避免重入板级单例。 */
    static LvAdapterSsd1677* Instance() {
        return instance_;
    }

    /** @brief 显示适配器是否可接受 UI 消息 */
    bool IsReady() const;

    /** @brief 板级初始化结束后请求开放UI，实际建屏由显示上下文执行 */
    void SetSystemReady();

    /** @brief 获取 UI 递归锁 */
    bool LockUi(int timeout_ms) {
        return Lock(timeout_ms);
    }

    /** @brief 释放 UI 递归锁 */
    void UnlockUi() {
        Unlock();
    }

    /** @brief 标记下一次提交使用完整刷新 */
    void RequestFullOnNextCommit();

    /** @brief 绑定状态栏，供共用页面使用 */
    void BindStatusBar(lv_obj_t* status, lv_obj_t* notification, lv_obj_t* network, lv_obj_t* battery,
                       lv_obj_t* battery_pct, lv_obj_t* mute);

    /** @brief 获取当前状态栏绑定 */
    EpdStatusBar GetBoundStatusBar() const;

    /**
     * @brief 注册盖板虚拟键及回调，由独立触摸采集任务交付事件。
     * @note 键名及回调上下文由调用方管理，注册存续期间须保持有效。
     */
    void RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count, TouchVirtualKeyEventCb cb,
                                  void* user_data = nullptr);

    /** @brief 将情绪状态转交前台百问页面；其他页面忽略。 */
    void SetEmotion(const char* emotion) override;

    /** @brief 更新状态栏文案并缓存正文，供通知结束后恢复。 */
    void SetStatus(const char* status) override;

    /** @brief 将会话消息转交百问页面处理。 */
    void SetChatMessage(const char* role, const char* content) override;

    /** @brief 设置并保存显示主题选择。 */
    void SetTheme(Theme* theme) override;

    /** @brief 显示临时状态栏通知，duration_ms 指定持续时间。 */
    void ShowNotification(const char* notification, int duration_ms = 3000) override;

    /** @brief 刷新当前绑定的网络、静音、电池和空闲状态控件。 */
    void UpdateStatusBar(bool update_all = false) override;

    /** @brief 切换省电状态对应的会话及情绪显示。 */
    void SetPowerSaveMode(bool on) override;

    /** @brief 绑定当前页面的状态栏控件，覆盖此前页面绑定。 */
    void BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery, lv_obj_t* status,
                           lv_obj_t* notification, lv_obj_t* low_battery_popup, lv_obj_t* battery_pct = nullptr);

    /**
     * @brief 状态栏尚未绑定时排队通知；首次 BindStatusWidgets 后显示
     * @param notification 文案；nullptr/空串清除排队
     * @param duration_ms 显示时长；≤0 表示不自动隐藏
     */
    void QueueStatusNotification(const char* notification, int duration_ms = 60000);

    /**
     * @brief 设置状态栏标题前缀；空指针或空字符串清除前缀。
     * @note 内部获取显示锁，调用方应在未持锁时调用。
     */
    void SetStatusTitlePrefix(const char* prefix);

    /** @brief 显示关机叠图或全屏壁纸并等待物理刷新完成，失败返回 false。 */
    bool ShowPoweredOffScreen();

    /** @brief 关机准备失败后恢复界面输入和刷新，须在界面上下文调用。 */
    void CancelPowerOff();

    /**
     * @brief 按资源名获取映射区数据，成功时输出只读指针及长度。
     * @note 数据由资源映射持有，调用方不得释放。
     */
    bool TryGetResource(const char* name, const uint8_t** mem, size_t* size) const;

    /** @brief 进入待机前排空刷新并暂存待机帧，随后由待机定格接口提交。 */
    void BeginStandbyEnterPaint();

    /** @brief 提交待机画面并冻结后续刷新，成功返回 true。 */
    bool ParkEpdForStandby();

    /** @brief 恢复待机冻结的刷新，并请求重新显示当前界面。 */
    void WakeEpdFromStandby();

    /** @brief 待机页面显示后暂停触摸采集，保留 LVGL 定时器运行。 */
    void PauseStandbyBackgroundWork();

    /** @brief 退出待机页面后恢复触摸采集。 */
    void ResumeStandbyBackgroundWork();

    /** @brief 请求下一帧完整刷新以清理局刷残影。 */
    void RequestNextFullRefresh();

    /**
     * @brief 下一帧强制双平面局刷：0x26=旧、0x24=新（PARTIAL）
     * @note 正文沉浸进出真 4 灰 AA 时由 BookScreen 调用；开 AA 再跟灰 Overlay，关则 seed 后回 BW 局刷
     */
    void RequestDualPlanePartial();

    /** @brief 排空在途刷新后令面板进入深度睡眠，关机图应已完成。 */
    void SleepEpdForPowerOff();

    /** @brief 触摸复位唤醒后通知适配器重新读取输入。 */
    void KickTouchInput();

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    void SetupUI();

    // 已持 DisplayLock（或 LVGL 任务内）；按 prefix 规则写入 status_label_，并缓存正文。
    void ApplyStatusTextLocked(const char* status);

    // 已持锁/LVGL 上下文：把缓存的网络/静音/电池/状态灌进当前绑定的控件（首帧同屏画出）。
    void RestoreStatusWidgetsLocked();

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* battery_pct_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;

    char status_title_prefix_[48] = {};
    char status_body_cache_[32] = {};     // 不含前缀的状态正文（如「待命」/「12:34」）
    char battery_pct_cache_[8] = {};      // 如「100%」
    int battery_level_cache_ = -1;        // 上次写入百分比，避免同值反复刷
    char pending_notification_[48] = {};  // Bind 前排队的状态栏通知
    int pending_notification_ms_ = 0;

    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    EpdFlushCtx* epd_flush_ctx_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;
    mmap_assets_handle_t resources_assets_ = nullptr;

    std::atomic<bool> system_ready_pending_{false};  // 板级初始化完成通知
    std::atomic<bool> system_ready_{false};          // UI上下文持有
    static LvAdapterSsd1677* instance_;
};

/** @brief 查询触摸采集任务记录的屏内按下状态，供过滤迟到事件。 */
bool TouchUiFingerIsDown(void);

/** @brief 查询是否仍有尚未交付给 LVGL 的按下或松开边沿。 */
bool TouchUiHasPendingEdges(void);

/** @brief 在 LVGL 上下文为下一次正文绘制登记水波纹过渡区域。 */
void EpdRequestReaderRipple(lv_obj_t* screen, int direction, int left, int top, int width, int height);

/** @brief 在 LVGL 上下文取消尚未执行的正文水波纹过渡。 */
void EpdCancelReaderRipple();
