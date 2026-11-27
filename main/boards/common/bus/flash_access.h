#pragma once

#include <mutex>
#include <esp_err.h>

/** @brief 保护需要连续缓存访问的操作；不暂停总线，不得跨网络或UI回调等待。 */
class FlashCacheGuard {
public:
    /** @brief 取得当前任务可重入的缓存访问门闩 */
    FlashCacheGuard();

private:
    std::lock_guard<std::recursive_mutex> lock_;  // 当前任务持有的缓存门闩
};

/** @brief 按缓存门闩、总线的顺序接纳Flash访问，同任务嵌套仅排空一次总线。 */
class FlashAccessGuard {
public:
    /** @brief 取得缓存门闩并等待共享总线可安全执行 Flash 操作 */
    FlashAccessGuard();

    /** @brief 结束当前 Flash 访问，最后一层访问退出时恢复总线 */
    ~FlashAccessGuard();
    FlashAccessGuard(const FlashAccessGuard&) = delete;
    FlashAccessGuard& operator=(const FlashAccessGuard&) = delete;

    /** @brief 返回接纳结果，成功为ESP_OK，失败保留底层错误码。 */
    esp_err_t status() const {
        return status_;
    }

private:
    FlashCacheGuard cache_;      // 先取得缓存门闩，析构时最后释放
    esp_err_t status_ = ESP_OK;  // 仅成功接纳的访问参加嵌套计数
};
