#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

inline int VerticalAdvance(uint32_t cp, int cell) {
    switch (cp) {
        case 0xFF0C: case 0x3001: case 0x3002:
            return (cell + 1) / 2;
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

// 仅记录最后一个合法断点；源偏移与 UTF-8 长度分开保存，兼容 GBK TXT。
// ponytail: 不可分字符超过整列容量时按容量换列，避免越界或无限回退。
struct VerticalBreakState {
    uint32_t previous = 0; // 前一码点，零表示列首
    uint32_t offset = 0; // 合法断点的源偏移
    size_t text_size = 0; // 合法断点前的 UTF-8 字节数
    bool valid = false; // 当前列存在合法断点

    void Observe(uint32_t cp, uint32_t source_offset, size_t bytes) {
        if (previous && VerticalCanBreak(previous, cp)) {
            offset = source_offset;
            text_size = bytes;
            valid = true;
        }
        previous = cp;
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
