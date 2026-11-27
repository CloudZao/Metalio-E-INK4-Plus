#pragma once

#include <cstdint>
#include <esp_err.h>

// ESP32-S31 SDK sleep compatibility and retained diagnostics.
// These entry points run with caches enabled; cache-off SDK wrappers stay private.

enum class S31SleepPhase : uint32_t {
    Admission = 1,
    HardwareParked = 2,
    WakeArmed = 3,
    BeforeSleep = 4,
    AfterSleep = 5,
    KeysRestored = 6,
    BeforeSdRestore = 7,
    BeforeBusRestore = 8,
    Restored = 9,
};

esp_err_t S31SleepInit();
void S31SleepCaptureReset();
void S31SleepReportReset();
void S31SleepBeginCycle();
void S31SleepCheckpoint(S31SleepPhase phase, unsigned applied);
esp_err_t S31SleepLightSleep(unsigned applied);
