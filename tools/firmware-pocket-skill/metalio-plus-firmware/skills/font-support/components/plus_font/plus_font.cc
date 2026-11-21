// SPDX-License-Identifier: MIT
#include "plus_font.h"
#include <climits>
#include <cstring>
#include <limits>

namespace plus_font {
namespace {
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }
int16_t I16(const uint8_t* p) {
    const int32_t value = U16(p);
    return static_cast<int16_t>(value < 32768 ? value : value - 65536);
}
uint32_t U32(const uint8_t* p) { return uint32_t(U16(p)) | (uint32_t(U16(p+2)) << 16); }
bool Scalar(uint32_t cp) { return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff); }
bool Coordinate(int64_t n) { return n >= INT32_MIN && n <= INT32_MAX; }

bool SurfaceValid(const Surface1bpp& s) {
    if (!s.data || s.width <= 0 || s.height <= 0 || !s.stride_bytes) return false;
    const uint64_t row = (uint64_t(s.width) + 7) / 8;
    return s.stride_bytes >= row && s.capacity_bytes / s.stride_bytes >= uint32_t(s.height);
}

// An invalid sequence consumes its first byte only. The next call handles any
// remaining bytes, making truncation/continuation errors deterministic.
uint32_t Decode(const char* text, std::size_t length, std::size_t* at, bool* invalid) {
    *invalid = false;
    const uint8_t first = static_cast<uint8_t>(text[(*at)++]);
    if (first < 128) return first;
    unsigned n = 0;
    uint32_t cp = 0;
    if (first >= 0xc2 && first <= 0xdf) { n = 1; cp = first & 31; }
    else if (first >= 0xe0 && first <= 0xef) { n = 2; cp = first & 15; }
    else if (first >= 0xf0 && first <= 0xf4) { n = 3; cp = first & 7; }
    else { *invalid = true; return '?'; }
    if (n > length - *at) { *invalid = true; return '?'; }
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t c = static_cast<uint8_t>(text[*at+i]);
        if ((c & 0xc0) != 0x80) { *invalid = true; return '?'; }
        cp = (cp << 6) | (c & 63);
    }
    if (!Scalar(cp) || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000)) { *invalid = true; return '?'; }
    *at += n;
    return cp;
}

void Paint(const Surface1bpp& s, const Glyph& g, int32_t x, int32_t baseline, bool black) {
    const int64_t left = int64_t(x) + g.x_offset;
    const int64_t top = int64_t(baseline) + g.y_offset;
    if (left >= s.width || top >= s.height || left + g.width <= 0 || top + g.height <= 0) return;
    const int64_t x0 = left < 0 ? -left : 0;
    const int64_t y0 = top < 0 ? -top : 0;
    const int64_t x1 = left + g.width > s.width ? int64_t(s.width)-left : g.width;
    const int64_t y1 = top + g.height > s.height ? int64_t(s.height)-top : g.height;
    const bool set = black == s.one_is_black;
    for (int64_t gy = y0; gy < y1; ++gy) {
        uint8_t* row = s.data + std::size_t(top+gy) * s.stride_bytes;
        for (int64_t gx = x0; gx < x1; ++gx) {
            const uint64_t bit = uint64_t(gy)*g.width + uint64_t(gx);
            if (!(g.bitmap[bit/8] & (0x80u >> (bit&7)))) continue;
            const std::size_t px = static_cast<std::size_t>(left+gx);
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (px&7));
            if (set) row[px/8] |= mask;
            else row[px/8] &= static_cast<uint8_t>(~mask);
        }
    }
}

TextResult Process(const Font& font, const Surface1bpp* surface, const char* text,
                   std::size_t length, int32_t x, int32_t baseline, bool ink_black) {
    TextResult r = {};
    r.status = Status::Ok;
    r.end_x = x;
    r.end_baseline = baseline;
    if (!font.valid()) { r.status = Status::InvalidFont; return r; }
    if (surface && !SurfaceValid(*surface)) { r.status = Status::InvalidSurface; return r; }
    if (!text && length) { r.status = Status::InvalidText; return r; }
    if (!length) return r;
    const Metrics metrics = font.metrics();
    r.lines = 1;
    r.height = metrics.ascent + metrics.descent;
    int32_t line_width = 0;
    std::size_t at = 0;
    while (at < length) {
        const std::size_t before = at;
        bool invalid = false;
        const uint32_t cp = Decode(text, length, &at, &invalid);
        if (cp == '\n' || cp == '\r') {
            if (cp == '\r' && at < length && text[at] == '\n') ++at;
            const int64_t next_baseline = int64_t(r.end_baseline) + metrics.line_height;
            const int64_t next_height = int64_t(r.height) + metrics.line_height;
            if (!Coordinate(next_baseline) || !Coordinate(next_height) ||
                r.lines == std::numeric_limits<std::size_t>::max()) {
                r.status = Status::CoordinateOverflow; return r;
            }
            ++r.lines;
            r.height = static_cast<int32_t>(next_height);
            r.end_baseline = static_cast<int32_t>(next_baseline);
            r.end_x = x;
            line_width = 0;
            r.bytes_consumed = at;
            continue;
        }
        Glyph glyph = {};
        bool missing = false;
        if (!font.Find(cp == '\t' ? ' ' : cp, &glyph)) {
            missing = true;
            // Open requires '?'; no unchecked use of an absent glyph.
            if (!font.Find('?', &glyph)) { r.status = Status::InvalidFont; return r; }
        }
        const int32_t advance = int32_t(glyph.advance) * (cp == '\t' ? 4 : 1);
        const int64_t next_x = int64_t(r.end_x) + advance;
        const int64_t next_width = int64_t(line_width) + advance;
        if (!Coordinate(next_x) || !Coordinate(next_width)) {
            r.bytes_consumed = before;
            r.status = Status::CoordinateOverflow; return r;
        }
        if (surface && cp != '\t') Paint(*surface, glyph, r.end_x, r.end_baseline, ink_black);
        if (invalid) ++r.invalid_utf8;
        if (missing) ++r.missing_glyphs;
        r.end_x = static_cast<int32_t>(next_x);
        line_width = static_cast<int32_t>(next_width);
        if (line_width > r.width) r.width = line_width;
        r.bytes_consumed = at;
    }
    return r;
}
} // namespace

Font::Font() : data_(nullptr), count_(0), spec_(), metrics_() {}

bool Font::Open(const uint8_t* data, std::size_t size) {
    data_ = nullptr;
    count_ = 0;
    spec_ = Spec{};
    metrics_ = Metrics{};
    if (!data || size < 64 || size > UINT32_MAX || std::memcmp(data, "NTF1", 4) ||
        U16(data+4) != 1 || U16(data+6) != 40 || U32(data+8) != 1 ||
        U32(data+16) != 0 || U32(data+20) != 40 || U32(data+24) != 64 ||
        U32(data+36) != size) return false;
    const uint32_t count = U32(data+12);
    const uint64_t start = 64 + uint64_t(count)*20;
    if (count < 2 || start > size || U32(data+28) != start || U32(data+32) != start) return false;
    const uint8_t* spec = data+40;
    const Spec s = {U16(spec), U16(spec+2), U16(spec+4)};
    const Metrics m = {I16(spec+8), I16(spec+10), I16(spec+12)};
    if (!s.pixels || s.pixels > 256 || s.weight < 1 || s.weight > 1000 ||
        !s.threshold || s.threshold > 255 || U16(spec+6) || U16(spec+14) ||
        m.ascent < 1 || m.descent < 0 || m.line_height < m.ascent + m.descent ||
        U32(spec+16) != 0 || U32(spec+20) != count) return false;
    bool question = false, space = false;
    uint32_t previous = 0;
    uint64_t next_bitmap = start;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = data + 64 + std::size_t(i)*20;
        const uint32_t cp = U32(e), offset = U32(e+4);
        const uint32_t width = U16(e+8), height = U16(e+10);
        const int32_t y = I16(e+14), advance = I16(e+16);
        const uint64_t bits = uint64_t(width)*height;
        const uint64_t bytes = (bits+7)/8;
        if (!Scalar(cp) || (i && cp <= previous) || advance < 0 || U16(e+18) ||
            -y > m.ascent || int64_t(y) + height > m.descent ||
            offset != next_bitmap || next_bitmap + bytes > size) return false;
        if ((bits&7) && bytes && (data[offset+bytes-1] & ((1u << (8-(bits&7)))-1))) return false;
        if (cp == '?') question = true;
        if (cp == ' ' && advance > 0) space = true;
        next_bitmap += bytes;
        previous = cp;
    }
    if (next_bitmap != size || !question || !space) return false;
    data_ = data;
    count_ = count;
    spec_ = s;
    metrics_ = m;
    return true;
}

bool Font::Find(uint32_t codepoint, Glyph* glyph) const {
    if (!glyph) return false;
    *glyph = Glyph{};
    if (!data_) return false;
    uint32_t low = 0, high = count_;
    while (low < high) {
        const uint32_t mid = low + (high-low)/2;
        if (U32(data_+64+std::size_t(mid)*20) < codepoint) low = mid+1;
        else high = mid;
    }
    if (low == count_) return false;
    const uint8_t* e = data_+64+std::size_t(low)*20;
    if (U32(e) != codepoint) return false;
    glyph->width = U16(e+8);
    glyph->height = U16(e+10);
    glyph->x_offset = I16(e+12);
    glyph->y_offset = I16(e+14);
    glyph->advance = I16(e+16);
    glyph->bitmap = data_+U32(e+4);
    return true;
}

bool Font::Contains(uint32_t codepoint) const { Glyph g = {}; return Find(codepoint, &g); }
TextResult MeasureText(const Font& font, const char* text, std::size_t length, int32_t x, int32_t baseline) {
    return Process(font, nullptr, text, length, x, baseline, true);
}
TextResult DrawText(const Font& font, const Surface1bpp& surface, const char* text,
                    std::size_t length, int32_t x, int32_t baseline, bool ink_black) {
    return Process(font, &surface, text, length, x, baseline, ink_black);
}
} // namespace plus_font
