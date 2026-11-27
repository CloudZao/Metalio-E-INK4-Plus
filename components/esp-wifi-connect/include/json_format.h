#pragma once
#include <cstddef>
#include <string>

/**
 * @brief 将 cJSON 生成的紧凑 JSON 转为两空格缩进，不改动字符串及转义内容
 * @note 仅接收已由 cJSON 正确序列化的内容。
 */
inline std::string JsonFormatIndented(const char* compact) {
    std::string text;
    size_t depth = 0;
    auto newline = [&] {
        text += '\n';
        text.append(depth * 2, ' ');
    };
    for (size_t i = 0; compact[i] != '\0'; ++i) {
        const char c = compact[i];
        if (c == '"') {
            text += c;
            while (compact[++i] != '\0') {
                text += compact[i];
                if (compact[i] == '\\') {
                    text += compact[++i];
                } else if (compact[i] == '"') {
                    break;
                }
            }
        } else if (c == '{' || c == '[') {
            text += c;
            ++depth;
            if (compact[i + 1] != '}' && compact[i + 1] != ']') {
                newline();
            }
        } else if (c == '}' || c == ']') {
            --depth;
            if (compact[i - 1] != '{' && compact[i - 1] != '[') {
                newline();
            }
            text += c;
        } else if (c == ',') {
            text += c;
            newline();
        } else if (c == ':') {
            text += ": ";
        } else {
            text += c;
        }
    }
    return text;
}
