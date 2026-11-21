#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace plus_services {
// The fence outlives every Lease. RequestStop closes admission and invalidates
// results; it does not kill threads. Workers must honor cancellation and bound IO.
// Declare a Lease BEFORE request-owned resources, or use RunTracked, so resource
// destructors finish BEFORE active count drops to zero.
class TaskFence {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : owner_(other.owner_), epoch_(other.epoch_) { other.owner_ = nullptr; }
        Lease& operator=(Lease&&) = delete;
        ~Lease() { if (owner_) owner_->Finish(); }
        explicit operator bool() const { return owner_ != nullptr; }
        bool Cancelled() const { return !owner_ || !owner_->Current(epoch_); }
        std::uint64_t Generation() const { return epoch_; }
    private:
        friend class TaskFence;
        Lease(TaskFence* owner, std::uint64_t epoch) : owner_(owner), epoch_(epoch) {}
        TaskFence* owner_ = nullptr;
        std::uint64_t epoch_ = 0;
    };
    TaskFence() = default;
    TaskFence(const TaskFence&) = delete;
    TaskFence& operator=(const TaskFence&) = delete;
    Lease TryStart() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return {};
        ++active_;
        return Lease(this, epoch_);
    }
    void RequestStop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (accepting_) { accepting_ = false; ++epoch_; }
    }
    bool WaitStopped(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (accepting_) return false; // observation is not a stop request
        return done_.wait_for(lock, timeout, [&] { return active_ == 0; });
    }
    bool Resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ != 0) return false;
        accepting_ = true;
        return true;
    }
    // fn must be bounded, nonblocking and must not reenter this fence.
    // Publishes while holding the same gate as cancellation, closing the
    // "checked current then cancel then publish stale result" race.
    template<class Publish> bool CommitIfCurrent(const Lease& lease, Publish fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lease.owner_ != this || !accepting_ || lease.epoch_ != epoch_) return false;
        fn();
        return true;
    }
private:
    bool Current(std::uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepting_ && epoch_ == epoch;
    }
    void Finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--active_ == 0) done_.notify_all();
    }
    mutable std::mutex mutex_;
    std::condition_variable done_;
    std::uint64_t epoch_ = 1;
    unsigned active_ = 0;
    bool accepting_ = true;
};
template<class Work> bool RunTracked(TaskFence& fence, Work work) {
    auto lease = fence.TryStart();
    if (!lease) return false;
    work(lease); // all objects owned INSIDE work must be destroyed on return
    return true;
}
// Called by the controller, never from one of the workers being stopped.
// cancel_devices requests/awaits the actual driver abort or shutdown. A false
// result or timeout keeps admission closed and prevents pretending save succeeded.
template<class CancelDevices, class Save>
bool StopThenSave(TaskFence& fence, std::chrono::milliseconds timeout,
                  CancelDevices cancel_devices, Save save) {
    fence.RequestStop();
    if (!cancel_devices() || !fence.WaitStopped(timeout)) return false;
    return save(); // leave closed; explicit controller Resume only after safe recovery
}
} // namespace plus_services

