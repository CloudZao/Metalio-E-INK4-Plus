#pragma once
#include <cstdint>

namespace plus_hal_detail {
// Used only by the early deep-sleep wake gate, before UI/light/business init.
// Unlike the runtime key, an already-held wake key is the gesture to measure.
class WakeHoldGate {
public:
    bool Sample(bool down, int64_t now_us) {
        if (!down) { Reset(); return false; }
        if (last_ < 0 || now_us < last_ || now_us - last_ > 120000)
            since_ = now_us;
        last_ = now_us;
        return since_ >= 0 && now_us - since_ >= 5000000;
    }
    void Reset() { since_ = last_ = -1; }
private:
    int64_t since_ = -1, last_ = -1;
};
} // namespace plus_hal_detail
