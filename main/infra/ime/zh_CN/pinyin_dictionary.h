#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// 中文输入法索引和词条均在 SD 上；每次只读取一个键的有界候选块。
class PinyinDictionary {
public:
    static constexpr size_t kMaxKeyBytes = 63;  // 拼音或英文组合串上限
    static constexpr size_t kMaxCandidateBytes = 32768;  // 单个键的候选块上限
    enum class Result {
        Ready,      // 查询完成，允许候选为空
        Missing,    // 词库文件不存在
        Invalid,    // 文件格式或索引边界无效
        IoError,    // 存储读写失败
        Cancelled,  // 查询已取消
    };

    /**
     * @brief 校验二进制头并查询一个键，空键仅校验文件
     * @param output 非空键须提供至少 kMaxCandidateBytes + 1 字节的缓冲区
     */
    static Result Lookup(FILE* file, const char* key, char* output, size_t& bytes,
                         bool (*cancelled)(void*) = nullptr, void* context = nullptr) {
        bytes = 0;
        unsigned char header[32];
        if (std::fseek(file, 0, SEEK_SET) || std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return Result::IoError;
        }
        const uint32_t count = Read32(header + 12);
        const uint32_t index = Read32(header + 16);
        const uint32_t data = Read32(header + 20);
        const uint32_t size = Read32(header + 24);
        if (std::memcmp(header, "PINYIN2\0", 8) || Read32(header + 8) != 2 || index != 32 ||
            count > 2000000 || data != uint64_t(index) + uint64_t(count) * 72 || size < data ||
            std::strlen(key) > kMaxKeyBytes) {
            return Result::Invalid;
        }
        if (std::fseek(file, 0, SEEK_END)) {
            return Result::IoError;
        }
        const long actual_size = std::ftell(file);
        if (actual_size < 0) {
            return Result::IoError;
        }
        if (static_cast<uint64_t>(actual_size) != size) {
            return Result::Invalid;
        }
        if (!key[0]) {
            return Result::Ready;
        }
        uint32_t begin = 0, end = count;
        while (begin < end) {
            if (cancelled && cancelled(context)) {
                return Result::Cancelled;
            }
            const uint32_t middle = begin + (end - begin) / 2;
            unsigned char record[72];
            if (std::fseek(file, index + middle * 72, SEEK_SET) ||
                std::fread(record, 1, sizeof(record), file) != sizeof(record)) {
                return Result::IoError;
            }
            if (!std::memchr(record, 0, 64)) {
                return Result::Invalid;
            }
            const int order = std::strcmp(reinterpret_cast<const char*>(record), key);
            if (order < 0) {
                begin = middle + 1;
            } else if (order > 0) {
                end = middle;
            } else {
                const uint32_t offset = Read32(record + 64);
                const uint32_t length = Read32(record + 68);
                if (offset < data || length > kMaxCandidateBytes || uint64_t(offset) + length > size) {
                    return Result::Invalid;
                }
                if (std::fseek(file, offset, SEEK_SET) || std::fread(output, 1, length, file) != length) {
                    return Result::IoError;
                }
                output[length] = '\0';
                bytes = length;
                return Result::Ready;
            }
        }
        output[0] = '\0';
        return Result::Ready;
    }

private:
    static uint32_t Read32(const unsigned char* data) {
        return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
    }
};
