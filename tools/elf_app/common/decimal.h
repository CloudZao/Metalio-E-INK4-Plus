#pragma once
#include <stdint.h>
#include <stddef.h>

static void AppUnsigned(char* out, unsigned value) {
    char digits[12]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (unsigned i = 0; i < n; ++i) out[i] = digits[n - i - 1];
    out[n] = 0;
}
static void AppFormatNumber(double value, char out[32]) {
    if (value == 0) { out[0] = '0'; out[1] = 0; return; }
    size_t n = 0;
    if (value < 0) { out[n++] = '-'; value = -value; }
    int exponent = 0;
    while (value >= 10) { value /= 10; ++exponent; }
    while (value < 1) { value *= 10; --exponent; }
    uint64_t rounded = (uint64_t)(value * 100000000000.0 + 0.5);
    if (rounded >= UINT64_C(1000000000000)) { rounded /= 10; ++exponent; }
    char digits[12];
    for (int i = 11; i >= 0; --i) { digits[i] = (char)('0' + rounded % 10); rounded /= 10; }
    unsigned count = 12;
    while (count > 1 && digits[count - 1] == '0') --count;
    if (exponent >= 12 || exponent < -6) {
        out[n++] = digits[0];
        if (count > 1) {
            out[n++] = '.';
            for (unsigned i = 1; i < count; ++i) out[n++] = digits[i];
        }
        out[n++] = 'e';
        out[n++] = exponent < 0 ? '-' : '+';
        AppUnsigned(out + n, (unsigned)(exponent < 0 ? -exponent : exponent));
        return;
    }
    if (exponent < 0) {
        out[n++] = '0'; out[n++] = '.';
        for (int i = -1; i > exponent; --i) out[n++] = '0';
        for (unsigned i = 0; i < count; ++i) out[n++] = digits[i];
    } else {
        for (int i = 0; i <= exponent; ++i) out[n++] = (unsigned)i < count ? digits[i] : '0';
        if ((unsigned)(exponent + 1) < count) {
            out[n++] = '.';
            for (unsigned i = (unsigned)exponent + 1; i < count; ++i) out[n++] = digits[i];
        }
    }
    out[n] = 0;
}
static double AppParseNumber(const char* text) {
    const int negative = *text == '-';
    if (negative) ++text;
    double value = 0, fraction = 0;
    for (; *text; ++text) {
        if (*text == '.') { fraction = 0.1; continue; }
        if (fraction) { value += (*text - '0') * fraction; fraction /= 10; }
        else value = value * 10 + (*text - '0');
    }
    return negative ? -value : value;
}
