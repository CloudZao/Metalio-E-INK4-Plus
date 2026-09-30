#include "txt_chapter.h"

#include "text_encoding.h"

namespace reader {
namespace {

bool IsAsciiDigit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

bool IsCnNumeral(uint32_t cp) {
    switch (cp) {
        case 0x96F6u:  // 零
        case 0x3007u:  // 〇
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
    // 章回节讲篇话集（与 tools/ebook chapter_detect 对齐）
    switch (cp) {
        case 0x7AE0u:  // 章
        case 0x56DEu:  // 回
        case 0x8282u:  // 节
        case 0x8BB2u:  // 讲
        case 0x7BC7u:  // 篇
        case 0x8BDAu:  // 话
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

bool MatchDiNUnit(const uint8_t* data, size_t len) {
    return DiNUnitPrefixLen(data, len) > 0;
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

bool MatchAsciiKeyword(const uint8_t* data, size_t len, const char* kw) {
    size_t i = 0;
    for (; kw[i] != '\0'; ++i) {
        if (i >= len || data[i] != static_cast<uint8_t>(kw[i])) {
            return false;
        }
    }
    if (i >= len) {
        return false;
    }
    // 关键词后需空白再跟数字/罗马数字
    uint32_t cp = 0;
    size_t n = Utf8Next(data + i, len - i, &cp);
    if (n == 0 || !IsWs(cp)) {
        return false;
    }
    i = SkipWs(data, len, i);
    if (i >= len) {
        return false;
    }
    n = Utf8Next(data + i, len - i, &cp);
    if (n == 0) {
        return false;
    }
    if (IsAsciiDigit(cp)) {
        return true;
    }
    // 粗罗马数字
    return cp == 'I' || cp == 'V' || cp == 'X' || cp == 'L' || cp == 'C' || cp == 'D' || cp == 'M' ||
           cp == 'i' || cp == 'v' || cp == 'x';
}

bool MatchEnglishHeading(const uint8_t* data, size_t len) {
    static const char* kWords[] = {"Chapter", "CHAPTER", "chapter", "Part", "PART", "part",
                                   "Volume",  "VOLUME",  "Book",    "BOOK", "Act",  "ACT",
                                   "Section", "SECTION", nullptr};
    for (int w = 0; kWords[w] != nullptr; ++w) {
        if (MatchAsciiKeyword(data, len, kWords[w])) {
            return true;
        }
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
        // 「序」单字过宽：仅当整行就是「序」时已在 kb==len 返回
        if (kw[0] == static_cast<char>(0xE5) && kw[1] == static_cast<char>(0xBA) &&
            kw[2] == static_cast<char>(0x8F) && kb == 3) {
            // "序" UTF-8 E5 BA 8F — 拒绝「序言式」误伤已由更长词优先；裸「序」仅整行
            continue;
        }
        return false;
    }
    return false;
}

bool MatchNumericShort(const uint8_t* data, size_t len) {
    // ^[0-9]{1,4}(\S.*)?$ 且总长已由外层限制；排除「1.xxx」编辑说明
    size_t i = 0;
    size_t digits = 0;
    while (i < len && digits < 4) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, len - i, &cp);
        if (n == 0 || !IsAsciiDigit(cp)) {
            break;
        }
        i += n;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    if (i >= len) {
        return true;  // 纯数字章名「11」
    }
    uint32_t cp = 0;
    const size_t n = Utf8Next(data + i, len - i, &cp);
    if (n == 0) {
        return false;
    }
    // 排除英文列表「1.」/「1、」后接说明
    if (cp == '.' || cp == 0x3001u || cp == 0xFF0Eu) {
        return false;
    }
    return true;
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
    if (utf8 == nullptr || len < 1 || len > 60) {
        return false;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(utf8);
    size_t n = len;
    StripWrap(data, n);
    if (n < 1 || n > 60) {
        return false;
    }

    // 第N章/回/节… / 第N卷/部
    if (MatchDiNUnit(data, n)) {
        return true;
    }
    if (MatchJuanN(data, n)) {
        return true;
    }
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
