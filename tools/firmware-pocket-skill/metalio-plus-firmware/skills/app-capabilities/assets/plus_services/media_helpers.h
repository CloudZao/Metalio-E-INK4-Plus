#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
namespace plus_services {
// Plus Bluetooth chip reference conversion: 32-bit microphone slot, shift12, clamp.
// Floor division explicitly preserves the arithmetic-shift result for negatives.
inline std::int16_t MicSlotToPcm16(std::int32_t slot) {
    const std::int64_t x = slot;
    std::int64_t value = x >= 0 ? x / 4096 : -((-x + 4095) / 4096);
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    return static_cast<std::int16_t>(value);
}
inline std::int32_t Pcm16ToPlaybackSlot(std::int16_t sample, unsigned volume) {
    if (volume > 100) volume = 100;
    const std::int64_t factor = static_cast<std::int64_t>(volume) * volume * 65536 / 10000;
    const std::int64_t value = static_cast<std::int64_t>(sample) * factor;
    return static_cast<std::int32_t>(value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : value));
}
// Successful write means queued, never acoustically complete. After this helper
// the caller still needs the driver's true DMA drain/completion before stopping PA.
template<class Write, class Cancelled>
bool WriteAllPcm(Write write, Cancelled cancelled, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    if (!p && bytes) return false;
    while (bytes) {
        if (cancelled()) return false;
        std::size_t written = 0;
        if (!write(p, bytes, &written) || written == 0 || written > bytes) return false;
        p += written; bytes -= written;
    }
    return !cancelled();
}
// Actual renderer helper for final-resolution assets: white composite first,
// then black core. gray/alpha are 0..255; mask represents selected original contour.
inline std::uint8_t CompositeLineArt(std::uint8_t gray, std::uint8_t alpha, bool black_core) {
    const unsigned white = (static_cast<unsigned>(gray) * alpha + 255u * (255u - alpha) + 127u) / 255u;
    return black_core ? 0 : static_cast<std::uint8_t>(white);
}
inline std::uint8_t CompositeGray4(std::uint8_t gray, std::uint8_t alpha, bool black_core) {
    if (gray > 15) gray = 15;
    const unsigned white = (static_cast<unsigned>(gray) * alpha + 15u * (255u - alpha) + 127u) / 255u;
    return black_core ? 0 : static_cast<std::uint8_t>(white);
}
inline bool ChangedPixelsAreBinary(const std::uint8_t* old_frame,
                                   const std::uint8_t* next_frame, std::size_t pixels, std::uint8_t white = 255) {
    if (!old_frame || !next_frame) return false;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (old_frame[i] == next_frame[i]) continue;
        if ((old_frame[i] != 0 && old_frame[i] != white) ||
            (next_frame[i] != 0 && next_frame[i] != white)) return false;
    }
    return true; // assumes driver really skips unchanged pixels
}
// One in-flight source frame. Controller advances only after physical receipt.
// Timeout/failure leaves the pending frame visible to error handling; no fake FPS.
class FrameReceipt {
public:
    bool Submitted(std::uint64_t revision) {
        if (pending_ || revision == 0 || revision <= last_) return false;
        pending_ = revision; return true;
    }
    bool Complete(std::uint64_t displayed_revision, bool display_healthy) {
        if (!pending_ || !display_healthy || displayed_revision < pending_) return false;
        last_ = pending_; pending_ = 0; return true;
    }
    bool Pending() const { return pending_ != 0; }
private:
    std::uint64_t pending_ = 0, last_ = 0;
};
} // namespace plus_services

