#pragma once

#include "http.h"

#include <atomic>
#include <memory>
#include <mutex>

/**
 * @brief HTTP 落盘下载可取消门闩
 * @note UI 先 RequestCancel() 置位；AbortBoundHttp() 才能打断阻塞 Read。
 * @note AbortBoundHttp / RequestStop 勿在持 LVGL 锁时调用：Http::Close→Disconnect 会 join 接收任务，
 *       易在 lv_timer_handler 内把定时器链踩坏（InstructionFetchError）。
 * @note 关键修复：这里必须持有 Http 的真实所有权，不能保留裸指针，否则会出现悬空 Http*，
 *       进而在 DownloadGate::AbortBoundHttp() 中访问已释放对象并触发 LoadProhibited。
 */
struct DownloadGate {
    std::atomic<bool> cancel{false};
    std::mutex mu;
    std::unique_ptr<Http> http;

    void Reset() {
        cancel.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mu);
        http.reset();
    }

    /** 仅置取消位；可在 LVGL 线程调用 */
    void RequestCancel() {
        cancel.store(true, std::memory_order_release);
    }

    /** Close 当前绑定 Http；必须在非 LVGL 任务调用 */
    void AbortBoundHttp() {
        std::unique_ptr<Http> bound;
        {
            std::lock_guard<std::mutex> lock(mu);
            bound = std::move(http);
        }
        if (bound != nullptr) {
            bound->Close();
        }
    }

    /** 置位并 Close；仅可在非 LVGL 路径调用 */
    void RequestStop() {
        RequestCancel();
        AbortBoundHttp();
    }

    /**
     * @brief 绑定；若已取消则 Close 并返回 false
     */
    bool BindHttp(std::unique_ptr<Http> h) {
        if (h == nullptr) {
            return false;
        }
        if (cancel.load(std::memory_order_acquire)) {
            h->Close();
            return false;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (cancel.load(std::memory_order_acquire)) {
            h->Close();
            return false;
        }
        http = std::move(h);
        return true;
    }

    void UnbindHttp() {
        std::lock_guard<std::mutex> lock(mu);
        http.reset();
    }

    /**
     * @brief 下载任务收尾：仍持有绑定时 Close；已被 Abort 摘走则不再 Close
     */
    void CloseHttp() {
        std::unique_ptr<Http> bound;
        {
            std::lock_guard<std::mutex> lock(mu);
            bound = std::move(http);
        }
        if (bound != nullptr) {
            bound->Close();
        }
    }

    bool IsCancelled() const {
        return cancel.load(std::memory_order_acquire);
    }
};
