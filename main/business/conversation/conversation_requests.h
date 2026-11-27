#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

struct ConversationStartRequest {
    uint32_t generation;  // 请求所属会话代次
    bool pending;         // 是否存在待处理请求
    bool listen;          // 建立会话后是否请求聆听
};

class ConversationRequests {
public:
    /** @brief 查询当前代次，供异步回调判断是否仍然有效 */
    uint32_t Generation() const {
        return generation_.load(std::memory_order_acquire);
    }

    /** @brief 记录启动意图，合并同一代次中尚未兑现的聆听请求 */
    void Start(bool listen) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = true;
        listen_ = listen_ || listen;
    }

    /** @brief 将仍属于当前代次的启动请求留待重试 */
    void Defer(uint32_t generation, bool listen) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != Generation()) {
            return;
        }
        pending_ = true;
        listen_ = listen_ || listen;
    }

    /** @brief 原子取得请求参数与所属代次，取消不能留下半份请求 */
    ConversationStartRequest TakeStart() {
        std::lock_guard<std::mutex> lock(mutex_);
        const ConversationStartRequest request{Generation(), pending_, pending_ && listen_};
        if (pending_) {
            pending_ = false;
            listen_ = false;
        }
        return request;
    }

    /** @brief 取消当前会话，清理其全部待处理意图 */
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        pending_ = false;
        listen_ = false;
    }

    /** @brief 记录已建立会话中的聆听请求 */
    void Listen(uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == Generation()) {
            listen_ = true;
        }
    }

    /** @brief 取走仍有效的聆听请求 */
    bool TakeListen(uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != Generation()) {
            return false;
        }
        const bool listen = listen_;
        listen_ = false;
        return listen;
    }

    /** @brief 松手后撤回尚未兑现的聆听意图 */
    void CancelListen() {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_ = false;
    }

    /** @brief 查询系统暂停前是否存在尚未兑现的启动意图 */
    bool HasStart() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

private:
    std::mutex mutex_;                     // 保护复合请求，回调判代只读取原子值
    std::atomic<uint32_t> generation_{0};  // 取消时递增
    bool pending_ = false;                 // 待建立会话
    bool listen_ = false;                  // 待开始聆听
};
