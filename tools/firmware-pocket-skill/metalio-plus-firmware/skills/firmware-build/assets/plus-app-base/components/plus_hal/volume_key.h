#pragma once
#include <cstdint>

namespace plus_hal_detail {
// Successful samples from the same TCA read as the power key. 30 ms debounce,
// press-once then 450 ms initial / 180 ms held repeats. A blocked/unknown gap,
// transition or two-key chord requires both buttons released before rearming.
class VolumeKeyState {
public:
    void ResetUnknown() { *this = VolumeKeyState{}; }
    int Sample(bool up, bool down, bool blocked, uint32_t epoch, int64_t now) {
        if (blocked || epoch != epoch_ || (last_ >= 0 && (now < last_ || now - last_ > 120000))) {
            ResetUnknown(); epoch_ = epoch;
            if (blocked) return 0;
        }
        last_ = now;
        if (up && down) { ResetUnknown(); epoch_ = epoch; last_ = now; return 0; }
        const int raw = up ? 1 : down ? -1 : 0;
        if (!candidate_known_ || candidate_ != raw) {
            candidate_known_ = true; candidate_ = raw; since_ = now;
        }
        if (now - since_ < 30000) return 0;
        if (!stable_known_ || stable_ != candidate_) {
            stable_known_ = true; stable_ = candidate_;
            if (!stable_) { armed_ = true; next_repeat_ = -1; return 0; }
            if (!armed_) return 0;
            next_repeat_ = since_ + 450000;
            return stable_;
        }
        if (armed_ && stable_ && raw == stable_ && next_repeat_ >= 0 && now >= next_repeat_) {
            next_repeat_ = now + 180000;
            return stable_;
        }
        return 0;
    }
private:
    bool candidate_known_ = false, stable_known_ = false, armed_ = false;
    int candidate_ = 0, stable_ = 0;
    uint32_t epoch_ = 0;
    int64_t since_ = 0, last_ = -1, next_repeat_ = -1;
};
} // namespace plus_hal_detail
