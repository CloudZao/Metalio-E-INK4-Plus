#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace reader {

inline int VerticalAdvance(uint32_t cp, int cell, int glyph_advance = 0) {
    switch (cp) {
        case ',': case '.': case 0xFF0C: case 0x3001: case 0x3002:
            return (cell + 1) / 2;
        case ' ': case '\t': case 0xA0:
            return glyph_advance > 0 ? glyph_advance : (cell + 1) / 2;
        default: return cell;
    }
}

inline bool VerticalOpening(uint32_t cp) {
    switch (cp) {
        case '(': case '[': case '{': case 0xFF08: case 0xFF3B: case 0xFF5B:
        case 0x2018: case 0x201C: case 0x3008: case 0x300A: case 0x300C:
        case 0x300E: case 0x3010: case 0x3014: case 0x3016: case 0x3018:
        case 0x301A: case 0xFE41: case 0xFE43:
            return true;
        default: return false;
    }
}

inline bool VerticalClosing(uint32_t cp) {
    switch (cp) {
        case ',': case '.': case ':': case ';': case '!': case '?':
        case ')': case ']': case '}': case 0xFF09: case 0xFF3D: case 0xFF5D:
        case 0xFF0C: case 0x3001: case 0x3002: case 0xFF1A: case 0xFF1B:
        case 0xFF01: case 0xFF1F: case 0x2019: case 0x201D: case 0x3009:
        case 0x300B: case 0x300D: case 0x300F: case 0x3011: case 0x3015:
        case 0x3017: case 0x3019: case 0x301B: case 0xFE42: case 0xFE44:
            return true;
        default: return false;
    }
}

inline bool VerticalCanBreak(uint32_t before, uint32_t after) {
    return !VerticalOpening(before) && !VerticalClosing(after) &&
        !(before == after && (before == 0x2014 || before == 0x2026));
}

// 按每处实际空白累计收紧容量，保留 1px 净空及标题加粗的 1px 余量。
struct TextSpacingState {
    int capacity = 0; // 当前行可安全收紧的总像素数
    int tail_blank = 0; // 末字符为小标点时可收回的尾部空白
    int advance = 0; // 上一有墨迹字形至当前字符的累计步进
    int right = 0; // 上一有墨迹字形的右边界，不含边界像素
    int gaps = 0; // 上一有墨迹字形之后的间隔数
    int slots = 0; // 可用于两端对齐的间隔数
    bool ink = false; // 已遇到非空字形
    bool seen = false; // 已遇到字符
    bool known = true; // 当前行的字形边界均已知

    void Observe(int glyph_advance, int offset, int box_width, bool glyph_known,
                 int gap_limit, bool expand, int trailing_blank = 0) {
        tail_blank = glyph_known ? trailing_blank : 0;
        if (seen && expand) ++slots;
        seen = true;
        known = known && glyph_known;
        if (!known) capacity = 0;
        if (box_width > 0) {
            if (ink && known) {
                capacity += std::min(std::max(0, advance + offset - right - 2), gap_limit * gaps);
            }
            ink = true;
            right = offset + box_width;
            advance = glyph_advance;
            gaps = 1;
        } else if (ink) {
            advance += glyph_advance;
            ++gaps;
        }
    }
};

inline bool CanFitTrailingPunctuation(uint32_t cp, int width, int available,
                                      const TextSpacingState& spacing) {
    return (VerticalClosing(cp) || cp == 0x2014 || cp == 0x2026) &&
        width > available && width - available <= spacing.capacity + spacing.tail_blank;
}

// 仅记录最后一个合法断点；源偏移与 UTF-8 长度分开保存，兼容 GBK TXT。
// ponytail: 不可分字符超过整行或整列容量时按容量换行或换列，避免越界或无限回退。
struct VerticalBreakState {
    size_t glyphs = 0; // 当前行或列的码点数，不含段首缩进
    uint32_t previous = 0; // 前一码点，零表示行首或列首
    uint32_t offset = 0; // 合法断点的源偏移
    size_t text_size = 0; // 合法断点前的 UTF-8 字节数
    bool valid = false; // 当前行或列存在合法断点

    void Observe(uint32_t cp, uint32_t source_offset, size_t bytes) {
        if (previous && VerticalCanBreak(previous, cp)) {
            offset = source_offset;
            text_size = bytes;
            valid = true;
        }
        previous = cp;
        ++glyphs;
    }
};

inline uint32_t VerticalGlyph(uint32_t cp) {
    switch (cp) {
        case 0x201C: return 0x300E;
        case 0x201D: return 0x300F;
        case 0x2018: return 0x300C;
        case 0x2019: return 0x300D;
        default: return cp;
    }
}

inline bool VerticalRotate(uint32_t cp) {
    switch (cp) {
        case '(': case ')': case '[': case ']': case '{': case '}':
        case 0xFF08: case 0xFF09: case 0xFF3B: case 0xFF3D: case 0xFF5B: case 0xFF5D:
        case 0x3008: case 0x3009: case 0x300A: case 0x300B: case 0x300C: case 0x300D:
        case 0x300E: case 0x300F: case 0x3010: case 0x3011: case 0x3014: case 0x3015:
        case 0x3016: case 0x3017: case 0x3018: case 0x3019: case 0x301A: case 0x301B:
        case 0x2014: case 0x2026: return true;
        default: return false;
    }
}

} // namespace reader
