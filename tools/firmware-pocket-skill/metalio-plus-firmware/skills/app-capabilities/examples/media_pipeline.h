#pragma once
#include "media_helpers.h"
#include <cstddef>
#include <cstdint>
namespace plus_examples {
// Production-format math: the right/reference slot is deliberately not mixed in.
// Input/output buffers belong to caller; these operations do not allocate DMA.
inline void ExtractMicrophone(const std::int32_t* stereo, std::int16_t* mono, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i)
        mono[i] = plus_services::MicSlotToPcm16(stereo[i * 2]);
}
inline void MakePlayback(const std::int16_t* mono, std::int32_t* stereo,
                         std::size_t frames, unsigned volume) {
    for (std::size_t i = 0; i < frames; ++i)
        stereo[i * 2] = stereo[i * 2 + 1] = plus_services::Pcm16ToPlaybackSlot(mono[i], volume);
}
// Pass actual final-size gray/alpha/selected contour to this helper. Asset
// preview and device renderer should call the same formula, not approximate CSS.
inline void CompositeAsset(const std::uint8_t* gray, const std::uint8_t* alpha,
                            const std::uint8_t* black_core, std::uint8_t* pixels, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        pixels[i] = plus_services::CompositeLineArt(gray[i], alpha[i], black_core[i] != 0);
}
}

