#pragma once

#include "chat_memory.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>

// 接收端仅更新缓存；快照与句尾在同一锁内投递，保持文本和状态变更顺序。
class YunxinTextInbox {
public:
    struct Snapshot {
        ChatString text; // 本次展示的文本副本
        uint32_t generation; // 所属会话代号
        bool finish; // 是否为句尾
        uint32_t sequence = 0; // 句尾确认序号，0 表示普通更新
        std::function<void()> after; // 文本应用后执行的同域状态变更
        bool queued = false; // 缓存内用于防止重复投递
    };

    /** @brief 更新接收缓存；queue 返回是否接受消息，不得等待业务执行 */
    template<class Queue>
    void Push(const char* text, Queue&& queue) {
        if (text == nullptr || text[0] == '\0') { return; }
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ == text) { return; }
        const size_t length = std::strlen(text);
        if (!latest_.empty() && length >= latest_.size() &&
            std::memcmp(text, latest_.data(), latest_.size()) == 0) {
            latest_.append(text + latest_.size(), length - latest_.size());
        } else {
            if (!latest_.empty()) {
                auto snapshot = Snapshot{std::move(latest_), generation_.load(), true, ++sequence_};
                RememberBoundary(snapshot);
                QueueBoundaries(queue);
                latest_.clear();
            }
            latest_.assign(text, length);
        }
        dirty_ = true;
    }

    /** @brief 重试未交付的句尾，再投递最新文本；队列暂满时保留缓存 */
    template<class Queue>
    void Update(Queue&& queue) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!QueueBoundaries(queue)) return;
        if (!dirty_) { return; }
        dirty_ = !queue(Snapshot{latest_, generation_.load(), false});
    }

    /** @brief 保存句尾及其状态变更，交付失败后由 Update 重试 */
    template<class Queue>
    void Finish(Queue&& queue, std::function<void()> after = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto snapshot = Snapshot{std::move(latest_), generation_.load(), true, ++sequence_, std::move(after)};
        RememberBoundary(snapshot);
        QueueBoundaries(queue);
        latest_.clear();
        dirty_ = false;
    }

    /** @brief 失效旧会话并取出残留文本，不再执行旧会话的状态变更 */
    ChatVector<Snapshot> Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto generation = generation_.fetch_add(1) + 1;
        auto result = std::move(pending_boundaries_);
        for (auto& snapshot : result) {
            snapshot.generation = generation;
            snapshot.after = {};
        }
        if (!latest_.empty()) {
            try { result.push_back(Snapshot{std::move(latest_), generation, true}); }
            catch (const std::bad_alloc&) { /* 内存不足时仍保留已经缓存的句尾。 */ }
        }
        latest_.clear();
        dirty_ = false;
        return result;
    }

    /** @brief 判断快照是否仍属于当前会话 */
    bool IsCurrent(const Snapshot& snapshot) const {
        return snapshot.generation == generation_.load();
    }

    /** @brief 确认句尾已应用并归还缓存 */
    void Applied(const Snapshot& snapshot) {
        if (!snapshot.finish || snapshot.sequence == 0) { return; }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_boundaries_.begin(); it != pending_boundaries_.end(); ++it) {
            if (it->sequence == snapshot.sequence) { pending_boundaries_.erase(it); break; }
        }
    }

private:
    template<class Queue>
    bool QueueBoundaries(Queue& queue) {
        for (auto& boundary : pending_boundaries_) {
            if (!boundary.queued) {
                if (!queue(boundary)) return false;
                boundary.queued = true;
            }
        }
        return true;
    }
    void RememberBoundary(const Snapshot& snapshot) {
        // 与语音队列容量一致；持续拥塞时保留较新的句尾，避免缓存无界增长。
        if (pending_boundaries_.size() >= 32) pending_boundaries_.erase(pending_boundaries_.begin());
        pending_boundaries_.push_back(snapshot);
    }
    std::mutex mutex_; // 保护接收缓存和投递顺序
    ChatString latest_; // 当前句子的最新全文
    bool dirty_ = false; // 当前全文尚未交付
    uint32_t sequence_ = 0; // 句尾确认序号
    ChatVector<Snapshot> pending_boundaries_; // 有界的未确认句尾及其状态变更
    std::atomic<uint32_t> generation_{0}; // 会话失效代号
};
