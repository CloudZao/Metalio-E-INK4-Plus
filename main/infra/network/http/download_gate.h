#pragma once
#include <atomic>
#include <mutex>
#include "http.h"

/** @brief 工作任务拥有 HTTP 对象，门闩仅在绑定作用域内借用。 */
class DownloadGate {
public:
    /** @brief 无活动连接时清除取消状态。 */
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!http_) {
            cancel_.store(false);
        }
    }

    /** @brief 标记取消，可在 UI 上下文调用，不执行阻塞关闭。 */
    void RequestCancel() {
        cancel_.store(true);
    }

    /** @brief 中断已取消的请求，须在允许阻塞的清理任务调用。 */
    void AbortBoundHttp() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancel_.load() && http_) {
            http_->Cancel();
        }
    }

    /** @brief 由工作任务在请求返回后关闭连接，不销毁 HTTP 对象。 */
    void CloseHttp() {
        CloseBoundHttp(nullptr);
    }

    /** @brief 查询当前请求的取消状态。 */
    bool IsCancelled() const {
        return cancel_.load();
    }

    /** @brief 必须声明在 HTTP 所有者之后，确保先解除借用再销毁 HTTP 对象。 */
    class Binding {
    public:
        /** @brief 绑定可选门闩；close_on_exit 用于请求作用域统一关闭连接。 */
        Binding(DownloadGate* gate, Http* http, bool close_on_exit = false)
            : gate_(gate), http_(http), close_on_exit_(close_on_exit) {
            bound_ = http && (!gate || gate->BindHttp(http));
        }
        ~Binding() {
            if (!bound_ || !gate_) {
                return;
            }
            if (close_on_exit_) {
                gate_->CloseBoundHttp(http_);
            } else
                gate_->UnbindHttp(http_);
        }

        /** @brief 查询绑定是否成功，取消或重复占用时为 false。 */
        explicit operator bool() const {
            return bound_;
        }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;

    private:
        DownloadGate* gate_;  // 不拥有门闩
        Http* http_;          // 不拥有连接
        bool close_on_exit_;  // 工作任务退出作用域时关闭
        bool bound_ = false;  // 已成功绑定
    };

private:
    bool BindHttp(Http* http) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!http || http_ || cancel_.load()) {
            return false;
        }
        http_ = http;
        return true;
    }
    void UnbindHttp(Http* expected) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (http_ == expected) {
            http_ = nullptr;
        }
    }
    void CloseBoundHttp(Http* expected) {
        Http* bound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (expected && http_ != expected) {
                return;
            }
            bound = http_;
            http_ = nullptr;
        }
        if (bound) {
            bound->Close();
        }
    }
    std::atomic<bool> cancel_{false};  // 取消不等于工作任务已结束
    std::mutex mutex_;                 // 保护借用连接与销毁时序
    Http* http_ = nullptr;             // 工作任务持有的对象
};
