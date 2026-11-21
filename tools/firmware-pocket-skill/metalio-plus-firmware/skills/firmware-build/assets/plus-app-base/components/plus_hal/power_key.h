#pragma once
#include <cstdint>

namespace plus_hal_detail {
enum class PowerKeyEvent { None, ShortPress, LongPress };

// Successful hardware samples only. Unknown/failed intervals disarm until a
// confirmed release. A long hold is reported once and never also a short tap.
class PowerKeyState {
public:
    void ResetUnknown() { *this = PowerKeyState{}; }

    PowerKeyEvent Sample(bool down, int64_t now_us) {
        if (last_sample_ >= 0 && now_us - last_sample_ > kMaximumGapUs)
            ResetUnknown();
        last_sample_ = now_us;
        if (!candidate_known_ || down != candidate_down_) {
            candidate_known_ = true;
            candidate_down_ = down;
            candidate_since_ = now_us;
        }
        if (now_us - candidate_since_ >= kDebounceUs &&
            (!stable_known_ || stable_down_ != candidate_down_)) {
            stable_known_ = true;
            stable_down_ = candidate_down_;
            if (stable_down_) {
                down_since_ = candidate_since_;
                long_reported_ = false;
            } else {
                const bool was_armed = armed_;
                armed_ = true;
                if (was_armed && down_since_ >= 0 && !long_reported_) {
                    const int64_t duration = candidate_since_ - down_since_;
                    down_since_ = -1;
                    if (duration >= kLongUs) return PowerKeyEvent::LongPress;
                    if (duration >= kMinimumShortUs) return PowerKeyEvent::ShortPress;
                }
                down_since_ = -1;
            }
        }
        if (down && stable_known_ && stable_down_ && armed_ &&
            !long_reported_ && down_since_ >= 0 && now_us - down_since_ >= kLongUs) {
            long_reported_ = true;
            return PowerKeyEvent::LongPress;
        }
        return PowerKeyEvent::None;
    }
private:
    static constexpr int64_t kDebounceUs = 30000;
    static constexpr int64_t kMinimumShortUs = 60000;
    static constexpr int64_t kLongUs = 5000000;
    static constexpr int64_t kMaximumGapUs = 120000;
    bool candidate_known_ = false, candidate_down_ = false;
    bool stable_known_ = false, stable_down_ = false;
    bool armed_ = false, long_reported_ = false;
    int64_t candidate_since_ = 0, down_since_ = -1, last_sample_ = -1;
};
} // namespace plus_hal_detail
