// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace plus_font {

struct Spec { uint16_t pixels, weight, threshold; };
struct Metrics { int16_t ascent, descent, line_height; };
struct Glyph {
    uint16_t width, height;
    int16_t x_offset, y_offset, advance;
    const uint8_t* bitmap;  // Continuous MSB-first bits; 1=ink, no row padding.
};

// Non-owning, allocation-free view. Data must stay alive AND immutable until
// the last call using this Font or its Glyphs. Open validates the entire pack.
// Each NTF1 pack here has exactly one spec and no test-only catalog.
class Font {
public:
    Font();
    bool Open(const uint8_t* data, std::size_t size);
    bool valid() const { return data_ != nullptr; }
    Spec spec() const { return spec_; }
    Metrics metrics() const { return metrics_; }
    uint32_t glyph_count() const { return count_; }
    bool Find(uint32_t codepoint, Glyph* glyph) const; // Exact lookup, no fallback.
    bool Contains(uint32_t codepoint) const;
private:
    const uint8_t* data_;
    uint32_t count_;
    Spec spec_;
    Metrics metrics_;
};

// Rows are byte aligned, MSB-first. stride_bytes may exceed ceil(width/8).
// This differs from the tightly packed bits inside an individual Glyph.
struct Surface1bpp {
    uint8_t* data;
    std::size_t capacity_bytes;
    int32_t width, height;
    std::size_t stride_bytes;
    bool one_is_black;
};

enum class Status { Ok, InvalidFont, InvalidSurface, InvalidText, CoordinateOverflow };
struct TextResult {
    Status status;
    int32_t width;        // Largest line advance, NOT an ink bounding box.
    int32_t height;       // First line ascent+descent, then line_height per newline.
    int32_t end_x, end_baseline;
    std::size_t bytes_consumed, lines, missing_glyphs, invalid_utf8;
};

// No allocation, shaping, automatic wrapping, kerning, or scaling. Coordinates
// are device pixels. LF/CR/CRLF start a new line at initial x. TAB advances by
// four spaces. Missing glyphs use '?' and increment missing_glyphs; malformed
// UTF-8 uses '?' and increments invalid_utf8 (one invalid byte at a time).
// Length is explicit; null text is allowed only for length=0.
TextResult MeasureText(const Font& font, const char* text, std::size_t length,
                       int32_t x=0, int32_t baseline=0);

// Transparent drawing: only ink bits are touched. Caller clears old contents.
// Pixels outside the surface are clipped; width/height still describe layout.
// ink_black=false supports white ink on an already-painted black background.
TextResult DrawText(const Font& font, const Surface1bpp& surface,
                    const char* text, std::size_t length,
                    int32_t x, int32_t baseline, bool ink_black=true);

} // namespace plus_font
