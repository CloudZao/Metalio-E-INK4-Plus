#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <climits>

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

// 横排优先压缩字距以容纳尾部标点；竖排使用合法断点回退。
// 字形包围框之间保留 1px 净空，并额外预留标题向右加粗的 1px。
struct TextSpacingState {
    int limit = INT_MAX; // 所有已见字间隔允许的统一收紧量
    int advance = 0; // 上一有墨迹字形至当前字符的累计步进
    int right = 0; // 上一有墨迹字形的右边界，不包含边界像素
    int gaps = 0; // 上一有墨迹字形之后的字间隔数
    bool ink = false; // 已遇到非空字形

    void Observe(int glyph_advance, int offset, int box_width, bool known = true) {
        if (!known) limit = 0;
        if (box_width > 0) {
            if (ink) limit = std::min(limit, std::max(0, (advance + offset - right - 2) / gaps));
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

inline int PunctuationLetterSpace(int width, int available, size_t glyphs, int cell,
                                   int blank_limit) {
    if (width <= available || glyphs < 2) return 0;
    const int gaps = static_cast<int>(glyphs - 1);
    const int reduction = (width - available + gaps - 1) / gaps;
    const int limit = cell / 12 > 0 ? cell / 12 : 1; // 每字间距最多收紧约字框的 8%，至少 1px
    return reduction <= std::min(limit, blank_limit) ? -reduction : 0;
}

inline bool CanFitTrailingPunctuation(uint32_t cp, int width, int available,
                                      size_t glyphs, int cell, int blank_limit) {
    return (VerticalClosing(cp) || cp == 0x2014 || cp == 0x2026) &&
        PunctuationLetterSpace(width, available, glyphs, cell, blank_limit) < 0;
}

// 仅记录最后一个合法断点；源偏移与 UTF-8 长度分开保存，兼容 GBK TXT。
// ponytail: 不可分字符超过整列容量时按容量换列，避免越界或无限回退。
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
