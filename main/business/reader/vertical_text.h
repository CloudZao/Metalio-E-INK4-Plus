#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

static inline bool ReaderEnglishLetter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

struct ReaderEnglishBreak {
    size_t prefix = 0; // 当前行保留的源字节数，不含显示用连字符
    bool move_word = false; // 整个单词移至下一行
    bool hyphen = false; // 分词处追加显示用连字符
};

static constexpr size_t kReaderEnglishSuffixLimit = 2; // 允许连字符拆分的剩余字母数上限

// ponytail: 按剩余字母数量拆词，不引入音节词典；单词超过整行时允许分段。
template <typename GlyphWidth>
static ReaderEnglishBreak ReaderPlanEnglishBreak(std::string_view word, int available,
                                                size_t source_capacity, bool empty_line,
                                                const GlyphWidth& glyph_width) {
    const int hyphen_width = glyph_width('-');
    size_t prefix = 0, prefix_letters = 0, letters = 0, plain_prefix = 0;
    int width = 0;
    for (size_t i = 0; i < word.size(); ++i) {
        width += glyph_width(static_cast<unsigned char>(word[i]));
        if (ReaderEnglishLetter(static_cast<unsigned char>(word[i]))) {
            ++letters;
        }
        if (width <= available && i + 1 <= source_capacity) {
            plain_prefix = i + 1;
        }
        if (width + hyphen_width <= available && i + 2 <= source_capacity &&
            ReaderEnglishLetter(static_cast<unsigned char>(word[i])) && i + 1 < word.size() &&
            ReaderEnglishLetter(static_cast<unsigned char>(word[i + 1]))) {
            prefix = i + 1;
            prefix_letters = letters;
        }
        // 超过行宽后不再测量后缀字形；只需判断剩余字母是否超过整词换行阈值。
        if (width > available || i + 1 > source_capacity) {
            if (!empty_line) {
                for (size_t j = i + 1; j < word.size() && letters - prefix_letters <= kReaderEnglishSuffixLimit; ++j) {
                    if (ReaderEnglishLetter(static_cast<unsigned char>(word[j]))) {
                        ++letters;
                    }
                }
            }
            break;
        }
    }
    if (plain_prefix == word.size()) {
        return {word.size(), false, false};
    }
    if (!empty_line && (prefix == 0 || letters - prefix_letters > kReaderEnglishSuffixLimit)) {
        return {0, true, false};
    }
    if (prefix != 0) {
        return {prefix, false, true};
    }
    return {std::max<size_t>(1, plain_prefix), false, false};
}

/** @brief 计算竖排字符步进，小标点采用半格，空白优先使用实际字体步进 */
inline int VerticalAdvance(uint32_t cp, int cell, int glyph_advance = 0) {
    switch (cp) {
        case ',':
        case '.':
        case 0xFF0C:
        case 0x3001:
        case 0x3002:
            return (cell + 1) / 2;
        case ' ':
        case '\t':
        case 0xA0:
            return glyph_advance > 0 ? glyph_advance : (cell + 1) / 2;
        default:
            return cell;
    }
}

/** @brief 识别不宜位于行末或列末的开括号及前引号 */
inline bool VerticalOpening(uint32_t cp) {
    switch (cp) {
        case '(':
        case '[':
        case '{':
        case 0xFF08:
        case 0xFF3B:
        case 0xFF5B:
        case 0x2018:
        case 0x201C:
        case 0x3008:
        case 0x300A:
        case 0x300C:
        case 0x300E:
        case 0x3010:
        case 0x3014:
        case 0x3016:
        case 0x3018:
        case 0x301A:
        case 0xFE41:
        case 0xFE43:
            return true;
        default:
            return false;
    }
}

/** @brief 识别不宜位于行首或列首的闭括号、后引号及标点 */
inline bool VerticalClosing(uint32_t cp) {
    switch (cp) {
        case ',':
        case '.':
        case ':':
        case ';':
        case '!':
        case '?':
        case ')':
        case ']':
        case '}':
        case 0xFF09:
        case 0xFF3D:
        case 0xFF5D:
        case 0xFF0C:
        case 0x3001:
        case 0x3002:
        case 0xFF1A:
        case 0xFF1B:
        case 0xFF01:
        case 0xFF1F:
        case 0x2019:
        case 0x201D:
        case 0x3009:
        case 0x300B:
        case 0x300D:
        case 0x300F:
        case 0x3011:
        case 0x3015:
        case 0x3017:
        case 0x3019:
        case 0x301B:
        case 0xFE42:
        case 0xFE44:
            return true;
        default:
            return false;
    }
}

/** @brief 按标点首尾禁则判断相邻码点之间是否允许换行或换列 */
inline bool VerticalCanBreak(uint32_t before, uint32_t after) {
    return !VerticalOpening(before) && !VerticalClosing(after) &&
           !(before == after && (before == 0x2014 || before == 0x2026));
}

// 按每处实际空白累计收紧容量，保留 1px 净空及标题加粗的 1px 余量。
struct TextSpacingState {
    int capacity = 0;    // 当前行可安全收紧的总像素数
    int tail_blank = 0;  // 末字符为小标点时可收回的尾部空白
    int advance = 0;     // 上一有墨迹字形至当前字符的累计步进
    int right = 0;       // 上一有墨迹字形的右边界，不含边界像素
    int gaps = 0;        // 上一有墨迹字形之后的间隔数
    int slots = 0;       // 可用于两端对齐的间隔数
    bool ink = false;    // 已遇到非空字形
    bool seen = false;   // 已遇到字符
    bool known = true;   // 当前行的字形边界均已知

    /** @brief 累计字形间可安全收紧的真实空白，未知边界时停止收紧并记录对齐间隔 */
    void Observe(int glyph_advance, int offset, int box_width, bool glyph_known, int gap_limit, bool expand,
                 int trailing_blank = 0) {
        tail_blank = glyph_known ? trailing_blank : 0;
        if (seen && expand) {
            ++slots;
        }
        seen = true;
        known = known && glyph_known;
        if (!known) {
            capacity = 0;
        }
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

/** @brief 判断超宽行能否通过安全收紧和收回尾部空白容纳末尾标点 */
inline bool CanFitTrailingPunctuation(uint32_t cp, int width, int available, const TextSpacingState& spacing) {
    return (VerticalClosing(cp) || cp == 0x2014 || cp == 0x2026) && width > available &&
           width - available <= spacing.capacity + spacing.tail_blank;
}

// 仅记录最后一个合法断点；源偏移与 UTF-8 长度分开保存，兼容 GBK TXT。
// ponytail: 不可分字符超过整行或整列容量时按容量换行或换列，避免越界或无限回退。
struct VerticalBreakState {
    size_t glyphs = 0;      // 当前行或列的码点数，不含段首缩进
    uint32_t previous = 0;  // 前一码点，零表示行首或列首
    uint32_t offset = 0;    // 合法断点的源偏移
    size_t text_size = 0;   // 合法断点前的 UTF-8 字节数
    bool valid = false;     // 当前行或列存在合法断点

    /** @brief 消费一个码点，保留最近合法断点的源偏移和输出文本字节数 */
    void Observe(uint32_t cp, uint32_t source_offset, size_t bytes, bool english_words = false) {
        if (previous && VerticalCanBreak(previous, cp) &&
            !(english_words && ((ReaderEnglishLetter(previous) && (ReaderEnglishLetter(cp) || cp == '\'')) ||
                (previous == '\'' && ReaderEnglishLetter(cp))))) {
            offset = source_offset;
            text_size = bytes;
            valid = true;
        }
        previous = cp;
        ++glyphs;
    }
};

/** @brief 将横排单双引号映射为竖排使用的直角引号，其他码点保持原值 */
inline uint32_t VerticalGlyph(uint32_t cp) {
    switch (cp) {
        case 0x201C:
            return 0x300E;
        case 0x201D:
            return 0x300F;
        case 0x2018:
            return 0x300C;
        case 0x2019:
            return 0x300D;
        default:
            return cp;
    }
}

/** @brief 查询竖排绘制时是否需要旋转括号、引号、破折号或省略号 */
inline bool VerticalRotate(uint32_t cp) {
    switch (cp) {
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case 0xFF08:
        case 0xFF09:
        case 0xFF3B:
        case 0xFF3D:
        case 0xFF5B:
        case 0xFF5D:
        case 0x3008:
        case 0x3009:
        case 0x300A:
        case 0x300B:
        case 0x300C:
        case 0x300D:
        case 0x300E:
        case 0x300F:
        case 0x3010:
        case 0x3011:
        case 0x3014:
        case 0x3015:
        case 0x3016:
        case 0x3017:
        case 0x3018:
        case 0x3019:
        case 0x301A:
        case 0x301B:
        case 0x2014:
        case 0x2026:
            return true;
        default:
            return false;
    }
}
