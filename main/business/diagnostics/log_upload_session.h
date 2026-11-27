#pragma once

#include <atomic>
#include <memory>
#include <mutex>

enum class LogUploadResult {
    Pending,             // 上传尚未结束
    Ok,                  // 上传成功
    Failed,              // 上传失败
    Cancelled,           // 已取消
    NoFile,              // 没有可上传的日志
    NetFailed,           // 网络不可用
    StorageUnavailable,  // 存储不可用
    NoMemory,            // 内存不足
};
enum class LogUploadPhase {
    Snapshot,        // 准备日志快照
    WaitingNetwork,  // 等待网络就绪
    Sending,         // 发送日志
};

struct LogUploadSession {
    std::atomic<unsigned> references{0};  // 共享会话的引用计数
    std::atomic<bool> cancel{false};      // 用户或系统请求取消
    std::atomic<bool> running{true};      // 文件、网络及占用均收尾后清空
    std::atomic<int> percent{0};          // 当前上传进度
    std::atomic<unsigned> jobs{0};        // 原子快照：低 8 位为总数，随后各 8 位为完成数和文件编号
    std::atomic<LogUploadPhase> phase{LogUploadPhase::Snapshot};    // 当前上传阶段
    std::atomic<LogUploadResult> result{LogUploadResult::Pending};  // 最终结果

    /** @brief 工作任务释放文件、网络、电源及激活占用后发布最终结果 */
    void Finish(LogUploadResult value) {
        result.store(cancel.load() ? LogUploadResult::Cancelled : value);
        running.store(false, std::memory_order_release);
    }
};

// 会话自带引用计数，单次 nothrow 分配，不再为共享所有权分配独立控制块。
class LogUploadSessionPtr {
public:
    /** @brief 创建空会话引用 */
    LogUploadSessionPtr() = default;

    /** @brief 接管会话的一份共享引用，空指针保持为空 */
    explicit LogUploadSessionPtr(LogUploadSession* p) : ptr_(p) {
        Retain();
    }

    /** @brief 增加一份共享会话引用 */
    LogUploadSessionPtr(const LogUploadSessionPtr& other) : ptr_(other.ptr_) {
        Retain();
    }

    /** @brief 转移会话引用并清空来源 */
    LogUploadSessionPtr(LogUploadSessionPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    /** @brief 归还会话引用，最后一份引用负责释放会话 */
    ~LogUploadSessionPtr() {
        Release();
    }

    /** @brief 用指定会话替换当前引用并归还原引用 */
    LogUploadSessionPtr& operator=(LogUploadSessionPtr other) noexcept {
        auto* old = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = old;
        return *this;
    }

    /** @brief 访问非空引用所持有的会话 */
    LogUploadSession* operator->() const {
        return ptr_;
    }

    /** @brief 获取非空引用所持有的会话对象 */
    LogUploadSession& operator*() const {
        return *ptr_;
    }

    /** @brief 查询引用是否持有会话 */
    explicit operator bool() const {
        return ptr_ != nullptr;
    }

private:
    void Retain() {
        if (ptr_) {
            ptr_->references.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void Release() {
        if (ptr_ && ptr_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete ptr_;
        }
    }
    LogUploadSession* ptr_ = nullptr;  // 本引用持有的共享会话
};

// 串行处理启动与取消，各会话独立持有状态；注册表不接管文件、网络、界面或阻塞清理。
class LogUploadSessions {
public:
    /** @brief 获取最近一次登记的会话引用，尚未登记时返回空 */
    LogUploadSessionPtr Current() {
        std::lock_guard<std::mutex> lock(mu_);
        return current_;
    }

    /** @brief 没有在途会话时登记新会话，空引用或已有任务运行时返回 false */
    bool Begin(const LogUploadSessionPtr& next) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!next) {
            return false;
        }
        if (current_ && current_->running.load(std::memory_order_acquire)) {
            return false;
        }
        current_ = next;
        return true;
    }

    /** @brief 请求取消仍在运行的会话，由工作任务完成资源收尾 */
    void Cancel() {
        std::lock_guard<std::mutex> lock(mu_);
        if (current_ && current_->running.load()) {
            current_->cancel.store(true);
        }
    }

private:
    std::mutex mu_;                // 保护当前会话登记与取消
    LogUploadSessionPtr current_;  // 注册表保留的共享引用
};
