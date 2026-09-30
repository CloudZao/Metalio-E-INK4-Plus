#pragma once

// WASM/桌面模拟器桩：与 IDF esp_timer_get_time() 同语义（微秒单调时钟）

#include <chrono>
#include <cstdint>

inline int64_t esp_timer_get_time() {
    using clock = std::chrono::steady_clock;
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
            .count());
}
