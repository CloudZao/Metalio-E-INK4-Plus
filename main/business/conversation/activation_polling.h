#pragma once
#include <atomic>
#include <mutex>
#include <string>

class ActivationPollGuard;

class ActivationPolling {
public:
    /** @brief 获取云端轮询占用与待绑定状态的唯一所有者 */
    static ActivationPolling& GetInstance();

    /** @brief 增加暂停占用，已有请求由调用方等待结束 */
    void Pause();

    /** @brief 归还一次暂停占用，最后一次归还触发重新检查 */
    void Resume();

    /** @brief 查询是否仍有暂停占用 */
    bool IsPaused() const;

    /** @brief 查询激活请求是否仍在运行 */
    bool IsBusy();

    /** @brief 请求在可运行时重新检查 */
    void RequestCheck();

    /** @brief 查询是否有尚未交付的检查请求；投递失败时保持请求 */
    bool HasCheckRequest() const;

    /** @brief 取走合并的检查请求 */
    bool TakeCheckRequest();

    /** @brief 保存待绑定验证码，内容改变时返回 true */
    bool SetCode(const std::string& code);

    /** @brief 获取可跨任务使用的验证码副本 */
    std::string Code();

    /** @brief 查询是否存在待绑定验证码 */
    bool HasCode();

private:
    friend class ActivationPollGuard;
    ActivationPolling() = default;
    bool TryBegin();
    void Finish();

    std::mutex mutex_;                          // 保护暂停计数和请求占用
    unsigned pause_count_ = 0;                  // 嵌套暂停占用
    bool busy_ = false;                         // 在途激活请求
    std::string code_;                          // 待绑定验证码，只用于界面显示
    std::atomic<bool> paused_{false};           // 快速查询暂停状态
    std::atomic<bool> check_requested_{false};  // 合并恢复及唤醒请求
};

class ActivationPollGuard {
public:
    /** @brief 尝试独占一次轮询；暂停或已有请求时不取得占用 */
    ActivationPollGuard();

    /** @brief 在请求作用域结束后归还占用 */
    ~ActivationPollGuard();
    ActivationPollGuard(const ActivationPollGuard&) = delete;
    ActivationPollGuard& operator=(const ActivationPollGuard&) = delete;

    /** @brief 查询当前作用域是否已取得轮询占用 */
    explicit operator bool() const {
        return acquired_;
    }

private:
    bool acquired_;  // 当前作用域是否拥有请求占用
};
