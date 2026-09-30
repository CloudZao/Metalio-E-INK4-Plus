#include "text_encoding.h"

#include <cstdint>
#include <cstring>

#include <esp_heap_caps.h>
#include <ff.h>

namespace reader {
namespace {

bool AppendUtf8Cp(uint32_t cp, Utf8Buffer& out) {
    if (cp < 0x80) {
        return out.Push(static_cast<char>(cp));
    }
    if (cp < 0x800) {
        return out.Push(static_cast<char>(0xC0 | (cp >> 6))) &&
               out.Push(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    if (cp < 0x10000) {
        return out.Push(static_cast<char>(0xE0 | (cp >> 12))) &&
               out.Push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) &&
               out.Push(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out.Push('?');
}

void AppendUtf8Cp(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back('?');
    }
}

bool AppendGbkPair(uint8_t b, uint8_t b2, Utf8Buffer& out) {
    const WCHAR uni = ff_oem2uni(static_cast<WCHAR>((static_cast<unsigned>(b) << 8) | b2), 936);
    if (uni == 0) {
        return out.Push('?');
    }
    return AppendUtf8Cp(uni, out);
}

void AppendGbkPair(uint8_t b, uint8_t b2, std::string& out) {
    const WCHAR uni = ff_oem2uni(static_cast<WCHAR>((static_cast<unsigned>(b) << 8) | b2), 936);
    if (uni == 0) {
        out.push_back('?');
    } else {
        AppendUtf8Cp(uni, out);
    }
}

}  // namespace

void Utf8Buffer::Reset() {
    if (data != nullptr) {
        heap_caps_free(data);
    }
    data = nullptr;
    len = 0;
    cap = 0;
}

bool Utf8Buffer::Reserve(size_t need_cap) {
    if (need_cap <= cap) {
        return true;
    }
    size_t ncap = cap == 0 ? 65536 : cap;
    while (ncap < need_cap) {
        ncap *= 2;
        if (ncap < need_cap) {
            // overflow guard
            if (ncap > (SIZE_MAX / 2)) {
                ncap = need_cap;
                break;
            }
        }
    }
    char* np = static_cast<char*>(heap_caps_realloc(data, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (np == nullptr) {
        np = static_cast<char*>(heap_caps_realloc(data, ncap, MALLOC_CAP_8BIT));
    }
    if (np == nullptr) {
        return false;
    }
    data = np;
    cap = ncap;
    return true;
}

bool Utf8Buffer::Ensure(size_t extra) {
    return Reserve(len + extra);
}

bool Utf8Buffer::Push(char c) {
    if (!Ensure(1)) {
        return false;
    }
    data[len++] = c;
    return true;
}

bool Utf8Buffer::Append(const char* s, size_t n) {
    if (n == 0) {
        return true;
    }
    if (!Ensure(n)) {
        return false;
    }
    std::memcpy(data + len, s, n);
    len += n;
    return true;
}

char* Utf8Buffer::Release(size_t* out_len) {
    if (out_len) {
        *out_len = len;
    }
    char* p = data;
    data = nullptr;
    len = 0;
    cap = 0;
    return p;
}

bool LooksLikeUtf8(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return true;
    }
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        return true;
    }
    size_t i = 0;
    size_t good = 0;
    size_t bad = 0;
    const size_t limit = len < 8192 ? len : 8192;
    while (i < limit) {
        const uint8_t c = data[i];
        if (c < 0x80) {
            ++i;
            ++good;
            continue;
        }
        size_t need = 0;
        if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            ++i;
            ++bad;
            continue;
        }
        if (i + need >= limit) {
            ++bad;
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= need; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (ok) {
            i += 1 + need;
            ++good;
        } else {
            ++i;
            ++bad;
        }
    }
    if (good + bad == 0) {
        return true;
    }
    return good >= bad * 2;
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

size_t GbkNext(const uint8_t* s, size_t len, uint32_t* cp) {
    if (s == nullptr || len == 0) {
        if (cp) {
            *cp = 0;
        }
        return 0;
    }
    const uint8_t b = s[0];
    if (b < 0x80) {
        if (cp) {
            *cp = b;
        }
        return 1;
    }
    if (len < 2) {
        if (cp) {
            *cp = static_cast<uint32_t>('?');
        }
        return 1;
    }
    const WCHAR uni = ff_oem2uni(static_cast<WCHAR>((static_cast<unsigned>(b) << 8) | s[1]), 936);
    if (cp) {
        *cp = uni == 0 ? static_cast<uint32_t>('?') : static_cast<uint32_t>(uni);
    }
    return 2;
}

bool AppendBytesToUtf8Buf(const uint8_t* data, size_t len, bool as_gbk, Utf8Buffer& out,
                          uint8_t* carry_byte, bool* has_carry) {
    if (has_carry == nullptr || carry_byte == nullptr) {
        return false;
    }

    if (data == nullptr || len == 0) {
        if (*has_carry) {
            if (!out.Push('?')) {
                return false;
            }
            *has_carry = false;
        }
        return true;
    }

    if (!as_gbk) {
        size_t i = 0;
        if (out.len == 0 && len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
            i = 3;
        }
        // 批量拷贝时去掉 \r：分段追加
        while (i < len) {
            size_t j = i;
            while (j < len && data[j] != '\r') {
                ++j;
            }
            if (j > i) {
                if (!out.Append(reinterpret_cast<const char*>(data + i), j - i)) {
                    return false;
                }
            }
            i = j + (j < len && data[j] == '\r' ? 1 : 0);
        }
        return true;
    }

    size_t i = 0;
    if (*has_carry) {
        if (!AppendGbkPair(*carry_byte, data[0], out)) {
            return false;
        }
        *has_carry = false;
        i = 1;
    }
    while (i < len) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            if (b != '\r') {
                if (!out.Push(static_cast<char>(b))) {
                    return false;
                }
            }
            ++i;
            continue;
        }
        if (i + 1 >= len) {
            *carry_byte = b;
            *has_carry = true;
            break;
        }
        if (!AppendGbkPair(b, data[i + 1], out)) {
            return false;
        }
        i += 2;
    }
    return true;
}

bool AppendBytesToUtf8(const uint8_t* data, size_t len, bool as_gbk, std::string& out,
                       uint8_t* carry_byte, bool* has_carry) {
    if (has_carry == nullptr || carry_byte == nullptr) {
        return false;
    }
    if (data == nullptr || len == 0) {
        if (*has_carry) {
            out.push_back('?');
            *has_carry = false;
        }
        return true;
    }
    if (!as_gbk) {
        size_t i = 0;
        if (out.empty() && len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
            i = 3;
        }
        for (; i < len; ++i) {
            if (data[i] != '\r') {
                out.push_back(static_cast<char>(data[i]));
            }
        }
        return true;
    }
    size_t i = 0;
    if (*has_carry) {
        AppendGbkPair(*carry_byte, data[0], out);
        *has_carry = false;
        i = 1;
    }
    while (i < len) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            if (b != '\r') {
                out.push_back(static_cast<char>(b));
            }
            ++i;
            continue;
        }
        if (i + 1 >= len) {
            *carry_byte = b;
            *has_carry = true;
            break;
        }
        AppendGbkPair(b, data[i + 1], out);
        i += 2;
    }
    return true;
}

bool BytesToUtf8(const uint8_t* data, size_t len, bool as_gbk, std::string& out) {
    out.clear();
    if (data == nullptr || len == 0) {
        return true;
    }
    if (as_gbk) {
        out.reserve(len + len / 2);
    } else {
        out.reserve(len);
    }
    uint8_t carry = 0;
    bool has_carry = false;
    if (!AppendBytesToUtf8(data, len, as_gbk, out, &carry, &has_carry)) {
        return false;
    }
    return AppendBytesToUtf8(nullptr, 0, as_gbk, out, &carry, &has_carry);
}

}  // namespace reader
