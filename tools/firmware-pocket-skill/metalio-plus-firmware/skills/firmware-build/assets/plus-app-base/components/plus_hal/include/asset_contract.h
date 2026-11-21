#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace plus_assets {
enum class Format : uint8_t { Mono1 = 1, Gray4 = 4 };
struct Descriptor { Format format; unsigned width; unsigned height; size_t bytes; };
constexpr size_t RequiredBytes(Format format, unsigned w, unsigned h) {
    return format == Format::Mono1 ? ((size_t(w) + 7) / 8) * h :
           format == Format::Gray4 ? ((size_t(w) + 1) / 2) * h : 0;
}
inline bool Validate(Descriptor d, size_t linked_bytes) {
    return d.width == 1216 && d.height == 684 && d.bytes != 0 &&
           d.bytes == RequiredBytes(d.format, d.width, d.height) && linked_bytes == d.bytes;
}
// Production EPDiy buffer: even X is the LOW nibble, white=15.
inline bool Decode(Descriptor d, const uint8_t* source, size_t linked_bytes,
                   uint8_t* destination, size_t capacity) {
    if (!source || !destination || !Validate(d, linked_bytes) || capacity < 415872) return false;
    if (d.format == Format::Gray4) { std::memcpy(destination, source, d.bytes); return true; }
    for (size_t pixel = 0; pixel < 1216u * 684u; pixel += 2) {
        const uint8_t first = (source[pixel / 8] & (128u >> (pixel & 7))) ? 0 : 15;
        const uint8_t second = (source[(pixel + 1) / 8] & (128u >> ((pixel + 1) & 7))) ? 0 : 15;
        destination[pixel / 2] = uint8_t(first | (second << 4));
    }
    return true;
}
} // namespace plus_assets
