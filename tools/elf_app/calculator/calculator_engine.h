#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../common/decimal.h"

enum CalcError { CALC_OK, CALC_DIV_ZERO, CALC_RANGE };
typedef struct {
    char input[32]; // 当前输入或格式化结果
    char expression[96]; // 当前算式
    double value; // 当前操作数，保留内部精度
    double accumulator; // 待运算的左操作数
    double repeat_rhs; // 重复等号使用的右操作数
    char pending; // 等待执行的运算符
    char repeat; // 上一次等号的运算符
    unsigned replace; // 下一次数字输入替换结果
    unsigned waiting; // 已选运算符，尚未输入右操作数
    unsigned input_limit; // 输入位数已达到上限
    enum CalcError error; // 运算错误
} Calculator;

static size_t CalcLength(const char* text) {
    size_t n = 0;
    while (text[n]) ++n;
    return n;
}
static void CalcCopy(char* out, size_t capacity, const char* text) {
    size_t n = 0;
    while (n + 1 < capacity && text[n]) { out[n] = text[n]; ++n; }
    out[n] = 0;
}
static void CalcAppend(char* out, size_t capacity, const char* text) {
    const size_t n = CalcLength(out);
    CalcCopy(out + n, capacity - n, text);
}
static void CalcReset(Calculator* c) {
    CalcCopy(c->input, sizeof(c->input), "0");
    c->expression[0] = 0;
    c->value = c->accumulator = c->repeat_rhs = 0;
    c->pending = c->repeat = 0;
    c->replace = c->waiting = c->input_limit = 0;
    c->error = CALC_OK;
}
static int CalcInRange(double value) {
    if (!(value > -1e100 && value < 1e100)) return 0;
    const double magnitude = value < 0 ? -value : value;
    return magnitude == 0 || magnitude >= 1e-99;
}
static const char* CalcOperator(char operation) {
    switch (operation) {
        case '+': return " + ";
        case '-': return " − ";
        case '*': return " × ";
        case '/': return " ÷ ";
        default: return "";
    }
}
static void CalcExpression(Calculator* c, double left, char operation, const char* right) {
    char number[32];
    AppFormatNumber(left, number);
    CalcCopy(c->expression, sizeof(c->expression), number);
    CalcAppend(c->expression, sizeof(c->expression), CalcOperator(operation));
    CalcAppend(c->expression, sizeof(c->expression), right);
}
static int CalcEvaluate(Calculator* c, double left, char operation, double right) {
    if (operation == '/' && right == 0) c->error = CALC_DIV_ZERO;
    else {
        double value = left;
        switch (operation) {
            case '+': value += right; break;
            case '-': value -= right; break;
            case '*': value *= right; break;
            case '/': value /= right; break;
        }
        if (!CalcInRange(value)) c->error = CALC_RANGE;
        else { c->value = value; AppFormatNumber(value, c->input); return 1; }
    }
    c->pending = c->repeat = 0;
    c->replace = 1; c->waiting = 0;
    return 0;
}
static void CalcBeginEntry(Calculator* c) {
    if (!c->replace) return;
    CalcCopy(c->input, sizeof(c->input), "0");
    c->value = 0;
    c->replace = c->waiting = 0;
    if (!c->pending) { c->expression[0] = 0; c->repeat = 0; }
}
static void CalcInput(Calculator* c, char key) {
    if (key == 'C') { CalcReset(c); return; }
    const int digit = key >= '0' && key <= '9';
    if (c->error) {
        if (digit || key == '.' || key == '<') CalcReset(c);
        else return;
    }
    c->input_limit = 0;
    if (digit || key == '.') {
        CalcBeginEntry(c);
        const size_t begin = c->input[0] == '-' ? 1 : 0;
        size_t length = CalcLength(c->input);
        if (key == '.') {
            for (size_t i = begin; i < length; ++i) if (c->input[i] == '.') return;
        } else {
            unsigned digits = 0;
            for (size_t i = begin; i < length; ++i) if (c->input[i] != '.') ++digits;
            if (c->input[begin] == '0') --digits;
            if (digits >= 12) { c->input_limit = 1; return; }
            if (length == begin + 1 && c->input[begin] == '0') length = begin;
        }
        c->input[length++] = key; c->input[length] = 0;
        c->value = AppParseNumber(c->input);
        if (c->pending) CalcExpression(c, c->accumulator, c->pending, c->input);
        return;
    }
    if (key == '<') {
        if (c->replace) return;
        size_t length = CalcLength(c->input);
        if (length > 1) c->input[--length] = 0;
        if (length == 1 && c->input[0] == '-') CalcCopy(c->input, sizeof(c->input), "0");
        c->value = AppParseNumber(c->input);
        if (c->pending) CalcExpression(c, c->accumulator, c->pending, c->input);
        return;
    }
    if (key == 'N') {
        if (c->waiting) CalcBeginEntry(c);
        const size_t length = CalcLength(c->input);
        if (c->input[0] == '-') {
            for (size_t i = 0; i < length; ++i) c->input[i] = c->input[i + 1];
        } else {
            for (size_t i = length + 1; i > 0; --i) c->input[i] = c->input[i - 1];
            c->input[0] = '-';
        }
        c->value = -c->value;
        if (c->pending) CalcExpression(c, c->accumulator, c->pending, c->input);
        else { c->expression[0] = 0; c->repeat = 0; }
        return;
    }
    if (key == '%') {
        if (c->waiting) CalcBeginEntry(c);
        const double percent = c->value / 100;
        const double value = c->pending == '+' || c->pending == '-' ? c->accumulator * percent : percent;
        if (!CalcInRange(value)) { c->error = CALC_RANGE; c->pending = c->repeat = 0; return; }
        if (c->pending) {
            CalcExpression(c, c->accumulator, c->pending, c->input);
            CalcAppend(c->expression, sizeof(c->expression), "%");
        } else {
            CalcCopy(c->expression, sizeof(c->expression), c->input);
            CalcAppend(c->expression, sizeof(c->expression), "%");
            c->repeat = 0;
        }
        c->value = value; AppFormatNumber(value, c->input);
        c->replace = 1; c->waiting = 0;
        return;
    }
    if (key == '=') {
        const char operation = c->pending ? c->pending : c->repeat;
        if (!operation) return;
        const double left = c->pending ? c->accumulator : c->value;
        const double right = c->pending ? (c->waiting ? c->accumulator : c->value) : c->repeat_rhs;
        if (!c->pending || c->waiting) {
            char number[32]; AppFormatNumber(right, number);
            CalcExpression(c, left, operation, number);
        }
        CalcAppend(c->expression, sizeof(c->expression), " =");
        if (!CalcEvaluate(c, left, operation, right)) return;
        c->repeat = operation; c->repeat_rhs = right;
        c->pending = 0; c->replace = 1; c->waiting = 0;
        return;
    }
    if (key != '+' && key != '-' && key != '*' && key != '/') return;
    if (c->pending && !c->waiting && !CalcEvaluate(c, c->accumulator, c->pending, c->value)) return;
    c->accumulator = c->value;
    c->pending = key; c->repeat = 0; c->replace = c->waiting = 1;
    CalcExpression(c, c->accumulator, key, "");
}
