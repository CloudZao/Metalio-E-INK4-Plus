#pragma once

#include <cstdint>
#include <string>

// Independent Plus hardware component. Application/LVGL code belongs to main.
namespace plus_hal {
constexpr int kNativeWidth = 1216;
constexpr int kNativeHeight = 684;
constexpr int kLogicalWidth = 684;
constexpr int kLogicalHeight = 1216;

// Must be the first app_main action. A deep-sleep wake stays dark and retains
// the sleep poster until a continuous 5-second power hold is verified. It may
// enter deep sleep again and therefore not return after a short/false wake.
void EarlyWakeGate();
bool Init(std::string* error = nullptr);
enum class LockscreenStyle { Application };
// May be configured before Init; changing style while locked redraws overlay.
void SetLockscreenStyle(LockscreenStyle style);

// Physical landscape, row-major 1 bpp, MSB first, 1=black. Only 800x480
// and native 1216x684 are accepted. The single owned slot is copied before
// return; false means no admission. Busy includes queued and physical work.
// The worker uses independent nearest-neighbour scaling along X/Y for 800x480.
bool QueueMono(const uint8_t* frame, int width, int height, bool full,
               uint32_t revision, std::string* error = nullptr);
// Native 4bpp, even-X low nibble, values 0..15; complete copied snapshot.
// Binary changes use DU; changed gray targets use non-flashing GL16.
bool QueueGray4(const uint8_t* frame, bool full, uint32_t revision, std::string* error=nullptr);
enum class DisplayMode { Auto, GrayFast, GrayQuality, MonoFast };
// Diagnostic-only explicit mode. MonoFast rejects any gray pixel rather than
// feeding unsupported intermediate targets to a black/white waveform.
bool QueueTestFrame(const uint8_t*, bool clear, uint32_t revision, DisplayMode,
                    std::string* error=nullptr);
bool DisplayBusy();
bool DisplayHealthy();
// Advances only after successful ink completion (or proven identical pixels).
uint32_t DisplayedRevision();

// Native logical portrait coordinates (684x1216). Choose exactly one consumer
// API per application: FIFO press edges, or pointer snapshot with short-tap
// press/release replay. Neither API performs I2C in the caller.
bool PollTouch(int* x, int* y);
// JSON counters only; never includes text, passwords or touched coordinates.
// Motor started/finished count driver calls, not measured mechanical motion.
std::string InputDiagnostics();
// Side BOOT/AI GPIO61: one debounced click per release, disabled in lock mode.
bool PollAiClick();
// Side volume P10/P11, active low: consume bounded +/- steps. Disabled and
// discarded while locked, paused, waking or shut down; holds repeat safely.
int PollVolumeDelta();
// P14 powers a shared TPS/audio rail. Disabling it while the app/display runs
// is refused; only the fully drained deep-sleep path may cut that rail.
bool SetAudioPower(bool enabled, std::string* error = nullptr);
// Drain/pause display, touch and aux hardware around the board's P14 reset.
// Never call while this task already owns a PauseForFlash lease. The settled
// shared rail is restored before normal frame admission resumes.
bool ResetAudioPower(std::string* error = nullptr);
bool SetAudioAmplifier(bool enabled, std::string* error = nullptr);
bool ReadPointer(int* x, int* y);
// Clears both delivery paths and requires a physical release before new input.
void InvalidateTouches();
void Pulse();  // A bounded 30 ms full-level pulse; no soft-PWM traffic.
bool TfMounted();  // Optional /sdcard mount; never formats, no USB gadget.
int BatteryPercent();  // Cached real gauge reading, 0..100; -1 means unknown.
// Power sampling continues independently of frame activity. A debounced short
// release toggles runtime standby; a >=5 s hold reports PowerOff once and never
// also short-toggles. Call this before the main-loop IsStandby branch.
bool PollPowerOff();
// Runtime standby preserves RAM/TF, displays an internal lockscreen, rejects
// new app frames/touch, and sets both frontlight PWM channels to zero. Already
// accepted app frames finish before overlay. Wake redraws the retained app
// snapshot, then admits new frames; overlay/restoration never advances the app
// DisplayedRevision. The minimum PWM is restored (cool 2/1023, 20 kHz).
// IsStandby remains true throughout dirty/active restoration; successful restore
// invalidates old touches and requires a release before new input can arm.
// Long hold reports the save/deep-sleep request without waking the light.
bool IsStandby();
// Changes on each sleep/wake transition. LVGL applications should reset their
// indev when this changes so a pre-standby PRESSED cannot click after wake.
uint32_t StandbyRevision();
// Monotonic physical user activity; background tasks never update this clock.
uint64_t LastUserActivityUs();
// Call only after application save succeeds and AI is cancelled. Displays the
// deep-sleep poster, physically waits for completion, unmounts TF, turns off
// light/rails and enters real ESP deep sleep. Failure refuses deep sleep.
// Successful deep sleep never returns; false means refused and UI restored.
bool PowerOff();

// Close display admission, drain accepted work, pause/drain touch, motor and
// shared I2C; retain both hardware gates until ResumeAfterFlash on SAME task.
// Before writing Flash, applications must await DisplayedRevision for any
// required confirmation frame, then require this function to return true.
// No recursive acquisition. Failure restores normal hardware admission.
bool PauseForFlash(uint32_t timeout_ms);
void ResumeAfterFlash();
} // namespace plus_hal
