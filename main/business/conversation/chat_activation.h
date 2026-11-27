#pragma once
#include <atomic>

class ChatActivation {
public:
    /** @brief 在内部 RAM 网络或启动线程刷新配置；本次确认就绪时返回 true */
    bool Refresh();

    /** @brief 查询配置是否已就绪且无需绑定 */
    bool IsReady() const;

    /** @brief 查询是否正在执行配置请求 */
    bool IsChecking() const;

    /** @brief 查询服务是否已明确要求绑定 */
    bool NeedsBinding() const;

private:
    enum class Phase {
        Pending,   // 等待下次可运行的配置请求
        Checking,  // 配置请求正在执行
        Ready,     // 配置与激活已经就绪
    };
    std::atomic<Phase> phase_{Phase::Pending};   // 配置请求生命周期
    std::atomic<bool> binding_required_{false};  // 与请求阶段独立的服务绑定要求
};
