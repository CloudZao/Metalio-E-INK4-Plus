#include "txt_chapter.h"

#include "text_encoding.h"

#include <cstring>

namespace reader {
namespace {

bool IsAsciiDigit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

bool IsCnNumeral(uint32_t cp) {
    switch (cp) {
        case 0x96F6u:  // 零
        case 0x3007u:  // 〇
        case 0x25CBu:  // ○
        case 0x4E00u:  // 一
        case 0x4E8Cu:  // 二
        case 0x4E09u:  // 三
        case 0x56DBu:  // 四
        case 0x4E94u:  // 五
        case 0x516Du:  // 六
        case 0x4E03u:  // 七
        case 0x516Bu:  // 八
        case 0x4E5Du:  // 九
        case 0x5341u:  // 十
        case 0x767Eu:  // 百
        case 0x5343u:  // 千
        case 0x4E07u:  // 万
        case 0x4E24u:  // 两
            return true;
        default:
            return false;
    }
}

bool IsNumeral(uint32_t cp) {
    return IsAsciiDigit(cp) || IsCnNumeral(cp);
}

bool IsWs(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x3000u;  // 全角空格
}

// 消耗一段数字（阿拉伯或中文），至少 1 个；返回消耗字节数，失败 0。
size_t ConsumeNumerals(const uint8_t* data, size_t len, size_t i) {
    size_t start = i;
    size_t n_cp = 0;
    while (i < len) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, len - i, &cp);
        if (n == 0 || !IsNumeral(cp)) {
            break;
        }
        i += n;
        ++n_cp;
        if (n_cp > 12) {
            return 0;
        }
    }
    return n_cp > 0 ? (i - start) : 0;
}

size_t SkipWs(const uint8_t* data, size_t len, size_t i) {
    while (i < len) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, len - i, &cp);
        if (n == 0 || !IsWs(cp)) {
            break;
        }
        i += n;
    }
    return i;
}

bool IsChapterUnit(uint32_t cp) {
    // 中文章节单位
    switch (cp) {
        case 0x7AE0u:  // 章
        case 0x56DEu:  // 回
        case 0x8282u:  // 节
        case 0x8BB2u:  // 讲
        case 0x7BC7u:  // 篇
        case 0x8BDDu:  // 话
        case 0x96C6u:  // 集
            return true;
        default:
            return false;
    }
}

bool IsVolumeUnit(uint32_t cp) {
    return cp == 0x5377u /*卷*/ || cp == 0x90E8u /*部*/;
}

// ^第 <ws>? <num> <ws>? <章|回|…> ；成功返回前缀字节长，否则 0
size_t DiNUnitPrefixLen(const uint8_t* data, size_t len) {
    size_t i = 0;
    uint32_t cp = 0;
    size_t n = Utf8Next(data + i, len - i, &cp);
    if (n == 0 || cp != 0x7B2Cu) {  // 第
        return 0;
    }
    i += n;
    i = SkipWs(data, len, i);
    const size_t num_n = ConsumeNumerals(data, len, i);
    if (num_n == 0) {
        return 0;
    }
    i += num_n;
    i = SkipWs(data, len, i);
    n = Utf8Next(data + i, len - i, &cp);
    if (n == 0 || !(IsChapterUnit(cp) || IsVolumeUnit(cp))) {
        return 0;
    }
    return i + n;
}

// ^卷 <ws>? <num> ；成功返回前缀字节长，否则 0
size_t JuanNPrefixLen(const uint8_t* data, size_t len) {
    size_t i = 0;
    uint32_t cp = 0;
    size_t n = Utf8Next(data + i, len - i, &cp);
    if (n == 0 || cp != 0x5377u) {  // 卷
        return 0;
    }
    i += n;
    i = SkipWs(data, len, i);
    const size_t num_n = ConsumeNumerals(data, len, i);
    return num_n > 0 ? (i + num_n) : 0;
}

bool MatchJuanN(const uint8_t* data, size_t len) {
    return JuanNPrefixLen(data, len) > 0;
}

bool IsNameSep(uint32_t cp) {
    return IsWs(cp) || cp == ':' || cp == 0xFF1Au /*：*/ || cp == '-' || cp == 0x2014u ||
           cp == 0x2013u || cp == 0x3001u /*、*/ || cp == 0xFF0Cu /*，*/ || cp == ',';
}

uint8_t AsciiLower(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

size_t ConsumeRoman(const uint8_t* data, size_t len, size_t i) {
    const size_t start = i;
    for (int n = 0; n < 3 && i < len && AsciiLower(data[i]) == 'm'; ++n) ++i;
    static const char* kDigits[] = {"cdm", "xlc", "ivx"};
    for (const char* digit : kDigits) {
        if (i >= len) break;
        if (AsciiLower(data[i]) == digit[0] && i + 1 < len &&
            (AsciiLower(data[i + 1]) == digit[1] || AsciiLower(data[i + 1]) == digit[2])) {
            i += 2;
            continue;
        }
        if (AsciiLower(data[i]) == digit[1]) ++i;
        for (int n = 0; n < 3 && i < len && AsciiLower(data[i]) == digit[0]; ++n) ++i;
    }
    return i - start;
}

bool MatchAsciiKeyword(const uint8_t* data, size_t len, const char* kw) {
    size_t i = 0;
    for (; kw[i] != '\0'; ++i) {
        if (i >= len || AsciiLower(data[i]) != kw[i]) return false;
    }
    uint32_t cp = 0;
    if (i >= len || Utf8Next(data + i, len - i, &cp) == 0 || !IsWs(cp)) return false;
    i = SkipWs(data, len, i);
    const size_t start = i;
    if (i < len && IsAsciiDigit(data[i])) {
        while (i < len && IsAsciiDigit(data[i])) ++i;
        if (i - start > 4) return false;
    } else {
        i += ConsumeRoman(data, len, i);
    }
    if (i == start) return false;
    if (i == len) return true;
    // 编号必须完整结束，拒绝 Introduction、IVory、1st 等普通单词。
    Utf8Next(data + i, len - i, &cp);
    return IsNameSep(cp) || cp == '.';
}

bool MatchEnglishHeading(const uint8_t* data, size_t len) {
    static const char* kWords[] = {"chapter", "part", "volume", "book", "act", "section"};
    for (const char* word : kWords) {
        if (MatchAsciiKeyword(data, len, word)) return true;
    }
    return false;
}

// 序章/楔子/番外…（整词前缀，后可接短副标题）
bool MatchSpecialHeading(const uint8_t* data, size_t len) {
    static const char* kWords[] = {
        "序章", "序言", "楔子", "引子", "引言", "前言", "绪论", "后记", "尾声", "终章",
        "番外篇", "番外", "外传", "附录", "导读", "内容简介", "跋", "序", nullptr};
    for (int w = 0; kWords[w] != nullptr; ++w) {
        const char* kw = kWords[w];
        size_t kb = 0;
        while (kw[kb] != '\0') {
            ++kb;
        }
        if (kb > len) {
            continue;
        }
        bool ok = true;
        for (size_t i = 0; i < kb; ++i) {
            if (data[i] != static_cast<uint8_t>(kw[i])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        if (kb == len) {
            return true;
        }
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + kb, len - kb, &cp);
        if (n == 0) {
            return false;
        }
        // 后接空白或常见分隔符
        if (IsWs(cp) || cp == ':' || cp == 0xFF1Au /*：*/ || cp == '-' || cp == 0x2014u ||
            cp == 0x2013u) {
            return true;
        }
        return false;
    }
    return false;
}

bool IsQuantityPrefix(const uint8_t* data, size_t len) {
    static const char* kChineseUnits[] = {
        "年", "月", "日", "岁", "个", "元", "秒", "分钟", "小时", "米", "厘米", "毫米",
        "公里", "公斤", "千克", "毫升", "升", "摄氏度"};
    for (const char* unit : kChineseUnits) {
        const size_t n = std::strlen(unit);
        if (len >= n && std::memcmp(data, unit, n) == 0) return true;
    }
    static const char* kAsciiUnits[] = {
        "kb", "mb", "gb", "tb", "kib", "mib", "gib", "hz", "khz", "mhz", "ghz",
        "v", "mv", "a", "ma", "mah", "w", "kw", "mm", "cm", "km", "kg", "ml"};
    for (const char* unit : kAsciiUnits) {
        size_t n = 0;
        while (unit[n] != '\0' && n < len && AsciiLower(data[n]) == unit[n]) ++n;
        if (unit[n] == '\0' &&
            (n == len || AsciiLower(data[n]) < 'a' || AsciiLower(data[n]) > 'z')) return true;
    }
    return false;
}

bool MatchNumericShort(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len && IsAsciiDigit(data[i])) ++i;
    if (i == 0 || i > 4) return false;
    if (i == len) return true;
    i = SkipWs(data, len, i);
    if (i == len || IsQuantityPrefix(data + i, len - i)) return false;
    uint32_t cp = 0;
    Utf8Next(data + i, len - i, &cp);
    if (cp == '(' || cp == 0xFF08u) {
        // 兼容仅用编号加括号附注的章名，不接受括号数字列表。
        const size_t n = Utf8Next(data + i, len - i, &cp);
        uint32_t first = 0, last = 0;
        if (i + n >= len) return false;
        Utf8Next(data + i + n, len - i - n, &first);
        size_t j = i;
        while (j < len) j += Utf8Next(data + j, len - j, &last);
        return !IsAsciiDigit(first) && (last == ')' || last == 0xFF09u);
    }
    // 数字章名必须跟文字，日期、时间、小数、列表标点及百分比均不匹配。
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0x3400u && cp <= 0x9FFFu) || (cp >= 0x20000u && cp <= 0x323AFu);
}

// 去掉首尾常见包裹符后的视图（不改原串，只调指针范围）
void StripWrap(const uint8_t*& data, size_t& len) {
    auto peel = [&](uint32_t open_cp, uint32_t close_cp) {
        if (len < 2) {
            return;
        }
        uint32_t a = 0, b = 0;
        const size_t na = Utf8Next(data, len, &a);
        if (na == 0 || a != open_cp) {
            return;
        }
        // 找最后码点
        size_t i = 0;
        size_t last = 0;
        size_t last_n = 0;
        while (i < len) {
            uint32_t cp = 0;
            const size_t n = Utf8Next(data + i, len - i, &cp);
            if (n == 0) {
                break;
            }
            last = i;
            last_n = n;
            b = cp;
            i += n;
        }
        if (b != close_cp || last_n == 0) {
            return;
        }
        data += na;
        len = last - na;
    };
    peel(0x3010u, 0x3011u);  // 【】
    peel(0x300Cu, 0x300Du);  // 「」
    peel(0x300Eu, 0x300Fu);  // 『』
    peel('(', ')');
    peel(0xFF08u, 0xFF09u);  // （）
}

}  // namespace

bool IsTxtChapterTitleLine(const char* utf8, size_t len) {
    if (utf8 == nullptr || len < 1 || len > kMaxTxtChapterTitleBytes) {
        return false;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(utf8);
    size_t n = len;
    StripWrap(data, n);
    if (n < 1) {
        return false;
    }
    size_t chars = 0;
    unsigned parentheses = 0;
    bool prose_punctuation = false;
    uint32_t previous = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp = 0;
        i += Utf8Next(data + i, n - i, &cp);
        if (++chars > kMaxTxtChapterTitleChars) return false;
        if (cp == '(' || cp == 0xFF08u) {
            ++parentheses;
        } else if ((cp == ')' || cp == 0xFF09u) && parentheses > 0) {
            --parentheses;
        } else if (parentheses == 0) {
            // 括号内可带作者附注；括号外的句末标点和分号视为正文特征。
            if (cp == 0x3002u || cp == ';' || cp == 0xFF1Bu) prose_punctuation = true;
            if (cp == '.' && i == n &&
                ((previous >= 'A' && previous <= 'Z') || (previous >= 'a' && previous <= 'z'))) {
                prose_punctuation = true;
            }
        }
        previous = cp;
    }

    // 明确序数允许标题标点及网页实体；紧接序数的语法成分视为正文引用。
    const size_t ordinal = DiNUnitPrefixLen(data, n);
    if (ordinal > 0) {
        uint32_t next = 0;
        Utf8Next(data + ordinal, n - ordinal, &next);
        if (prose_punctuation && (next == ',' || next == 0xFF0Cu)) return false;
        if (prose_punctuation) {
            static const char* kReferences[] = {"的", "中", "内", "里", "讲述", "介绍", "说明", "已经", "分内容"};
            for (const char* word : kReferences) {
                const size_t bytes = std::strlen(word);
                if (n - ordinal >= bytes && std::memcmp(data + ordinal, word, bytes) == 0) return false;
            }
        }
        return true;
    }
    if (MatchJuanN(data, n)) return true;
    if (prose_punctuation) return false;
    if (MatchSpecialHeading(data, n)) {
        return true;
    }
    if (MatchEnglishHeading(data, n)) {
        return true;
    }
    // 数字短章名（如「1清和宫上」）
    if (MatchNumericShort(data, n)) {
        return true;
    }
    return false;
}

bool SplitChapterHeading(const std::string& title, std::string& ordinal, std::string& name) {
    ordinal.clear();
    name.clear();
    if (title.empty()) {
        return false;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(title.data());
    size_t n = title.size();
    StripWrap(data, n);
    if (n < 1) {
        return false;
    }
    size_t prefix = DiNUnitPrefixLen(data, n);
    if (prefix == 0) {
        prefix = JuanNPrefixLen(data, n);
    }
    if (prefix == 0) {
        return false;
    }
    ordinal.assign(reinterpret_cast<const char*>(data), prefix);
    size_t i = prefix;
    while (i < n) {
        uint32_t cp = 0;
        const size_t cn = Utf8Next(data + i, n - i, &cp);
        if (cn == 0 || !IsNameSep(cp)) {
            break;
        }
        i += cn;
    }
    if (i < n) {
        name.assign(reinterpret_cast<const char*>(data + i), n - i);
        while (!name.empty() &&
               (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) {
            name.pop_back();
        }
    }
    return true;
}

std::string FormatChapterHeadingDisplay(const std::string& title) {
    std::string ordinal;
    std::string name;
    if (!SplitChapterHeading(title, ordinal, name)) {
        return title;
    }
    if (name.empty()) {
        return ordinal;
    }
    return ordinal + "\n" + name;
}

}  // namespace reader
