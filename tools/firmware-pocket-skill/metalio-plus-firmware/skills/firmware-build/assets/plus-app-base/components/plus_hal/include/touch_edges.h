#pragma once
#include <cstddef>
#include <cstdint>

namespace plus_hal_detail {
struct TouchEdge {
    int x = 0, y = 0;
    uint32_t epoch = 0;
    int64_t sampled_us = 0;
};
struct TouchEdgeStats {
    uint32_t presses = 0, releases = 0, enqueued = 0, delivered = 0;
    uint32_t overflow = 0, discarded = 0, invalidations = 0;
    uint32_t blocked_samples = 0, outside_samples = 0, max_delivery_us = 0;
};
struct TouchSampleResult { bool new_down = false, accepted = false; };

// Native button UI consumes DOWN only. Each accepted edge is retained in order;
// a hold/drag is one click. Caller holds the short input critical section.
template <size_t Capacity = 64> class TouchEdges {
public:
    static_assert(Capacity > 0);
    void Invalidate() {
        stats_.discarded += uint32_t(size_);
        ++stats_.invalidations;
        head_ = size_ = 0;
        down_ = false;
        wait_release_ = true;
        ++epoch_;
    }
    TouchSampleResult Sample(bool down, bool in_panel, int x, int y,
                             bool confirmed_release, bool blocked, int64_t now) {
        if (blocked) {
            ++stats_.blocked_samples;
            if (!wait_release_ || down_ || size_) Invalidate();
            return {};
        }
        if (!down) {
            // A recovered CONTACT->DOWN boundary can end a current gesture;
            // it must never open a layout/Flash/lock release gate.
            if (wait_release_ && !confirmed_release) return {};
            if (down_) ++stats_.releases;
            down_ = false;
            if (confirmed_release) wait_release_ = false;
            return {};
        }
        if (!in_panel) {
            ++stats_.outside_samples;
            wait_release_ = true;
            down_ = false;
            return {};
        }
        if (wait_release_) { ++stats_.blocked_samples; return {}; }
        if (down_) return {};
        down_ = true;
        ++stats_.presses;
        if (size_ == Capacity) {
            // Only DOWN records are queued: rejecting the newest does not
            // create an orphan UP or replace earlier accepted operations.
            ++stats_.overflow;
            return {true, false};
        }
        events_[(head_ + size_) % Capacity] = {x, y, epoch_, now};
        ++size_;
        ++stats_.enqueued;
        return {true, true};
    }
    bool Pop(TouchEdge* out, int64_t now) {
        if (!out || !size_) return false;
        *out = events_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        ++stats_.delivered;
        const int64_t delta = now - out->sampled_us;
        if (delta > int64_t(stats_.max_delivery_us))
            stats_.max_delivery_us = uint32_t(delta > UINT32_MAX ? UINT32_MAX : delta);
        return true;
    }
    bool WaitingRelease() const { return wait_release_; }
    bool Down() const { return down_; }
    size_t Size() const { return size_; }
    uint32_t Epoch() const { return epoch_; }
    TouchEdgeStats Stats() const { return stats_; }
private:
    TouchEdge events_[Capacity]{};
    size_t head_ = 0, size_ = 0;
    uint32_t epoch_ = 1;
    bool down_ = false, wait_release_ = true;
    TouchEdgeStats stats_{};
};

struct MotorRequest { uint32_t epoch = 0; int64_t requested_us = 0; };
inline bool MotorRequestLive(const MotorRequest& request, uint32_t epoch, bool blocked) {
    return !blocked && request.epoch == epoch;
}
}  // namespace plus_hal_detail
