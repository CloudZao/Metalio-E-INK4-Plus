#pragma once
#include <atomic>
#include <cstdint>

/** @brief 非阻塞投递到有界 UI 队列；成功后由 UI 回调接管参数 */
bool ScreenLvAsync(void (*cb)(void*), void* user_data = nullptr);

/** @brief 紧急投递 UI 任务 */
bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data = nullptr);

/** @brief 在持有 LVGL 锁的 Tick 中处理 UI 消息，与应用主循环独立 */
void ScreenLvAsyncDrain();

struct ScreenUiCompletion {
    ScreenUiCompletion* next = nullptr; // 交付前由生产者独占
    void (*apply)(ScreenUiCompletion*) = nullptr; // UI回调接管并释放对象，离页也须可安全执行
};

/** @brief 交付已接纳作业的完成对象；urgent用于资源关闭及待机就绪，不分配消息或等待队列空位。 */
void ScreenCompleteWork(ScreenUiCompletion* completion, bool urgent = false);

struct ScreenPaintCoalesce {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> done{0};
    std::atomic<bool> queued{false};
    void (*paint)() = nullptr;
    std::atomic<void*> defer_timer{nullptr};  // 允许 UI 与重试回调竞争创建，仅保留一个计时器
};

/** @brief 合并多次重绘请求，物理更新期间保留最新请求并延后重建 */
void ScreenPaintCoalesceRequest(ScreenPaintCoalesce* c);

/** @brief 停手后再刷 */
void ScreenPaintCoalesceRequestDebounced(ScreenPaintCoalesce* c, uint64_t delay_us);

/** @brief 重置合并重绘状态 */
void ScreenPaintCoalesceReset(ScreenPaintCoalesce* c);
