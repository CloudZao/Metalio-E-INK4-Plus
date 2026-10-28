#pragma once

#include "lvgl.h"

#include <atomic>

struct EpdStatusBar {
    lv_obj_t* bar = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* battery_pct_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_coord_t height = 0;
};

/** @brief 获取通用顶部状态栏高度 */
lv_coord_t ScreenStatusBarHeight();

/** @brief 创建通用状态栏 */
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr);
/**
 * @brief 在顶部状态栏下方显示黑底白字圆角提示，3 秒后自动消失
 * @param text 提示文本，调用时复制；空指针或空字符串不显示
 * @note 须在 LVGL 线程或持有 UI 锁时调用；连续调用替换提示并重新计时
 */
void ScreenShowToast(const char* text);
/** @brief 替换加载新屏 */
void ScreenLoadReplace(lv_obj_t* new_scr);
/** @brief 导航到工厂函数创建的新屏 */
void ScreenNavigateTo(lv_obj_t* (*create)());
/** @brief 返回上一屏 */
void ScreenNavigateBack();
/** @brief 标记当前是否为首页 */
void ScreenSetIsHome(bool is_home);
/** @brief 当前是否为首页 */
bool ScreenIsHome();
/** @brief 立即回到首页 */
void ScreenGoHome();
/** @brief 异步请求回首页 */
void ScreenRequestHome();
/** @brief 异步请求返回上一屏 */
void ScreenRequestBack();

/** @brief 非阻塞投递到有界 UI 队列；成功后由 UI 回调接管参数 */
bool ScreenLvAsync(void (*cb)(void*), void* user_data = nullptr);
/** @brief 紧急投递 UI 任务 */
bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data = nullptr);
/** @brief 在持有 LVGL 锁的 Tick 中处理 UI 消息，与应用主循环独立 */
void ScreenLvAsyncDrain();

struct ScreenPaintCoalesce {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> done{0};
    std::atomic<bool> queued{false};
    void (*paint)() = nullptr;
    std::atomic<void*> defer_timer{nullptr}; // 允许 UI 与重试回调竞争创建，仅保留一个计时器
    bool defer_while_epd_busy = false;
};

/** @brief 合并多次重绘请求 */
void ScreenPaintCoalesceRequest(ScreenPaintCoalesce* c);
/** @brief 停手后再刷 */
void ScreenPaintCoalesceRequestDebounced(ScreenPaintCoalesce* c, uint64_t delay_us);
/** @brief 重置合并重绘状态 */
void ScreenPaintCoalesceReset(ScreenPaintCoalesce* c);

/** @brief 应用网点背景样式；尺寸与位置由调用方布局管理，全局显示入口负责边界裁剪 */
void ScreenApplyDotBackdrop(lv_obj_t* obj);
