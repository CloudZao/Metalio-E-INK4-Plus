#pragma once
#include <cstdint>

namespace plus_hal_detail {
// One click on release. Boot-held keys, lock transitions and sampling gaps
// require a fresh released sample before another gesture is armed.
class AiKeyState {
public:
    void Reset() { *this = AiKeyState{}; }
    bool Sample(bool down, bool blocked, uint32_t epoch, int64_t now) {
        if (blocked || epoch != epoch_ || (last_ >= 0 && now-last_>150000)) {
            Reset(); epoch_=epoch;
            if (blocked) return false;
        }
        last_=now;
        if (!known_ || down!=candidate_) {
            known_=true; candidate_=down; since_=now;
        }
        if (now-since_<30000) return false;
        if (!stable_known_ || stable_!=candidate_) {
            const bool previously_down=stable_known_&&stable_;
            stable_known_=true;stable_=candidate_;
            if (stable_) { pressed_=since_; return false; }
            const bool click=armed_&&previously_down&&pressed_>=0&&since_-pressed_>=40000;
            armed_=true;pressed_=-1;return click;
        }
        return false;
    }
private:
    bool known_=false,candidate_=false,stable_known_=false,stable_=false,armed_=false;
    uint32_t epoch_=0;
    int64_t since_=0,last_=-1,pressed_=-1;
};
}
