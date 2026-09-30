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

/** @brief 创建通用状态栏 */
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr);
/** @brief 替换加载新屏 */
void ScreenLoadReplace(lv_obj_t* new_scr);
/** @brief 导航到工厂函数创建的新屏（本轮 stub） */
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

/** @brief 非 LVGL 线程投递 UI 任务 */
bool ScreenLvAsync(void (*cb)(void*), void* user_data = nullptr);
/** @brief 紧急投递 UI 任务 */
bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data = nullptr);

struct ScreenPaintCoalesce {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> done{0};
    std::atomic<bool> queued{false};
    void (*paint)() = nullptr;
    void* defer_timer = nullptr;
};

/** @brief 合并多次重绘请求 */
void ScreenPaintCoalesceRequest(ScreenPaintCoalesce* c);
/** @brief 停手后再刷 */
void ScreenPaintCoalesceRequestDebounced(ScreenPaintCoalesce* c, uint64_t delay_us);
/** @brief 重置合并重绘状态 */
void ScreenPaintCoalesceReset(ScreenPaintCoalesce* c);

/** @brief 为对象应用点状装饰底纹 */
void ScreenApplyDotBackdrop(lv_obj_t* obj);
