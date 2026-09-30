#pragma once

#include <cstdint>
#include <string>

namespace reader {

// 粗判：合法 UTF-8 比例够高则视为 UTF-8，否则按 GBK/GB2312 处理（国内 TXT 小说常见）。
bool LooksLikeUtf8(const uint8_t* data, size_t len);

// 将一段字节转为 UTF-8。as_gbk=true 时按 GBK 双字节解码。
bool BytesToUtf8(const uint8_t* data, size_t len, bool as_gbk, std::string& out);

// 可增长的 UTF-8 输出缓冲（优先 PSRAM），供大文件流式转码。
struct Utf8Buffer {
    char* data = nullptr;
    size_t len = 0;
    size_t cap = 0;

    void Reset();
    bool Reserve(size_t need_cap);
    bool Ensure(size_t extra);
    bool Push(char c);
    bool Append(const char* s, size_t n);
    // 移交所有权；之后 Reset 状态但不 free
    char* Release(size_t* out_len);
};

// 流式追加到 Utf8Buffer。结束时再调一次 data=nullptr 消化 GBK carry。
bool AppendBytesToUtf8Buf(const uint8_t* data, size_t len, bool as_gbk, Utf8Buffer& out,
                          uint8_t* carry_byte, bool* has_carry);

// 兼容：追加到 std::string
bool AppendBytesToUtf8(const uint8_t* data, size_t len, bool as_gbk, std::string& out,
                       uint8_t* carry_byte, bool* has_carry);

// 从 UTF-8 文本取下一个码点，返回字节长度；非法序列返回 1（跳过）。
size_t Utf8Next(const uint8_t* s, size_t len, uint32_t* cp);

// 从 GBK 取下一字符为 Unicode，返回源字节数（1 或 2）；非法返回 1 且 cp='?'。
size_t GbkNext(const uint8_t* s, size_t len, uint32_t* cp);

}  // namespace reader
