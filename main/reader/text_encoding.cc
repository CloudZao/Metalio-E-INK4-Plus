#include "text_encoding.h"

#include <ff.h>

namespace reader {
namespace {

struct Gb18030Range {
    uint16_t first; // 起始编码索引
    uint16_t last; // 结束编码索引，含边界
    uint16_t unicode; // 起始 Unicode 码点
};

#include "gb18030_tables.inc"

uint32_t LookupRange(const Gb18030Range* ranges, size_t count, uint32_t pointer) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const auto& range = ranges[mid];
        if (pointer < range.first) {
            hi = mid;
        } else if (pointer > range.last) {
            lo = mid + 1;
        } else {
            return range.unicode + pointer - range.first;
        }
    }
    return 0;
}

void AppendUtf8Cp(uint32_t cp, std::string& out) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

}  // namespace

bool LooksLikeUtf8(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return true;
    }
    const size_t limit = len < 8192 ? len : 8192;
    size_t i = 0;
    while (i < limit) {
        const uint8_t b = data[i];
        if (b < 0x80u) {
            ++i;
            continue;
        }
        const size_t need = b >= 0xC2u && b <= 0xDFu ? 2 :
                            b >= 0xE0u && b <= 0xEFu ? 3 :
                            b >= 0xF0u && b <= 0xF4u ? 4 : 0;
        if (need == 0) {
            return false;
        }
        for (size_t k = 1; k < need && i + k < limit; ++k) {
            if ((data[i + k] & 0xC0u) != 0x80u) {
                return false;
            }
        }
        if (i + 1 < limit &&
            ((b == 0xE0u && data[i + 1] < 0xA0u) ||
             (b == 0xEDu && data[i + 1] >= 0xA0u) ||
             (b == 0xF0u && data[i + 1] < 0x90u) ||
             (b == 0xF4u && data[i + 1] >= 0x90u))) {
            return false;
        }
        if (limit - i < need) {
            return true;
        }
        i += need;
    }
    return true;
}

size_t Utf8Next(const uint8_t* s, size_t len, uint32_t* cp) {
    if (s == nullptr || len == 0) {
        if (cp) {
            *cp = 0;
        }
        return 0;
    }
    const uint8_t c0 = s[0];
    if (c0 < 0x80) {
        if (cp) {
            *cp = c0;
        }
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80) {
        if (cp) {
            *cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) | (s[1] & 0x3F);
        }
        return 2;
    }
    if ((c0 & 0xF0) == 0xE0 && len >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        if (cp) {
            *cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) | (static_cast<uint32_t>(s[1] & 0x3F) << 6) |
                  (s[2] & 0x3F);
        }
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0 && len >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        if (cp) {
            *cp = (static_cast<uint32_t>(c0 & 0x07) << 18) | (static_cast<uint32_t>(s[1] & 0x3F) << 12) |
                  (static_cast<uint32_t>(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        }
        return 4;
    }
    if (cp) {
        *cp = static_cast<uint32_t>('?');
    }
    return 1;
}

size_t Gb18030Next(const uint8_t* s, size_t len, uint32_t* cp) {
    uint32_t uni = '?';
    size_t count = 1;
    if (s == nullptr || len == 0) {
        if (cp) *cp = 0;
        return 0;
    }
    const uint8_t b = s[0];
    if (b < 0x80u) {
        uni = b;
    } else if (b == 0x80u) {
        uni = 0x20ACu;
    } else if (b >= 0x81u && b <= 0xFEu && len >= 2) {
        const uint8_t b2 = s[1];
        if (b2 >= 0x30u && b2 <= 0x39u) {
            if (len == 2) {
                count = 2;
            } else if (s[2] >= 0x81u && s[2] <= 0xFEu) {
                if (len == 3) {
                    count = 3;
                } else if (s[3] >= 0x30u && s[3] <= 0x39u) {
                    count = 4;
                    const uint32_t pointer =
                        (((static_cast<uint32_t>(b - 0x81u) * 10 + b2 - 0x30u) * 126 +
                          s[2] - 0x81u) * 10 + s[3] - 0x30u);
                    uint32_t mapped = 0;
                    if (pointer >= 189000u && pointer <= 1237575u) {
                        mapped = 0x10000u + pointer - 189000u;
                    } else if (pointer == 7457u) {
                        mapped = 0xE7C7u;
                    } else if (pointer <= 39419u) {
                        mapped = LookupRange(kGb18030FourByteRanges,
                            sizeof(kGb18030FourByteRanges) / sizeof(kGb18030FourByteRanges[0]), pointer);
                    }
                    if (mapped != 0) uni = mapped;
                }
            }
        } else if (b2 >= 0x40u && b2 <= 0xFEu && b2 != 0x7Fu) {
            count = 2;
            const uint32_t pointer = static_cast<uint32_t>(b - 0x81u) * 190 +
                                     b2 - (b2 < 0x7Fu ? 0x40u : 0x41u);
            uint32_t mapped = LookupRange(kGb18030PairRanges,
                sizeof(kGb18030PairRanges) / sizeof(kGb18030PairRanges[0]), pointer);
            if (mapped == 0) {
                mapped = ff_oem2uni(static_cast<WCHAR>((static_cast<unsigned>(b) << 8) | b2), 936);
            }
            if (mapped != 0) uni = mapped;
        }
    }
    if (cp) *cp = uni;
    return count;
}

bool BytesToUtf8(const uint8_t* data, size_t len, bool as_gb18030, std::string& out) {
    out.clear();
    if (data == nullptr || len == 0) {
        return true;
    }
    out.reserve(as_gb18030 ? len + len / 2 : len);
    size_t i = 0;
    if (!as_gb18030 && len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        i = 3;
    }
    while (i < len) {
        if (!as_gb18030) {
            if (data[i] != '\r') out.push_back(static_cast<char>(data[i]));
            ++i;
            continue;
        }
        uint32_t cp = 0;
        i += Gb18030Next(data + i, len - i, &cp);
        if (cp != '\r') AppendUtf8Cp(cp, out);
    }
    return true;
}

}  // namespace reader
