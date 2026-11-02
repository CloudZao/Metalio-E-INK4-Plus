#pragma once

#include <mutex>

// A complete panel waveform needs uninterrupted cache/PSRAM access. Flash/NVS
// operations wait for it; nested accesses on the owning task share the same gate.
// Never hold this gate while waiting for LVGL callbacks or network data.
inline std::recursive_mutex& EpdFlashMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

class EpdFlashGuard {
public:
    EpdFlashGuard() : lock_(EpdFlashMutex()) {}

private:
    std::lock_guard<std::recursive_mutex> lock_;
};
