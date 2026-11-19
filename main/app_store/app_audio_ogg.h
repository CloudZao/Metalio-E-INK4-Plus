#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "app_api.h"

class AppAudioOggReader {
public:
    using Read = bool (*)(void*, uint64_t, void*, size_t); // 有界随机读取，不保留目标缓冲

    int Open(Read read, void* context, uint64_t size, uint64_t known_duration = 0) {
        read_ = read;
        context_ = context;
        size_ = size;
        offset_ = 0;
        sequence_ = 0;
        segments_ = index_ = 0;
        serial_known_ = eos_ = false;
        final_granule_ = UINT64_MAX;
        packet_.clear();
        int result = Next();
        if (result != XZ_APP_OK) return result;
        if (packet_.size() < 19 || std::memcmp(packet_.data(), "OpusHead", 8) != 0 ||
            packet_[8] != 1 || packet_[9] != 1 || packet_[18] != 0) return XZ_APP_ERR_UNSUPPORTED;
        pre_skip_ = U16(packet_.data() + 10);
        gain_ = static_cast<int16_t>(U16(packet_.data() + 16));
        result = Next();
        if (result != XZ_APP_OK) return result;
        if (packet_.size() < 16 || std::memcmp(packet_.data(), "OpusTags", 8) != 0) return XZ_APP_ERR_INVALID;
        if (known_duration) {
            duration_48k_ = known_duration;
            return XZ_APP_OK;
        }
        return FindDuration();
    }

    int Next() {
        packet_.clear();
        for (;;) {
            if (index_ == segments_) {
                if (eos_) return packet_.empty() ? 1 : XZ_APP_ERR_INVALID;
                const int result = LoadPage(!packet_.empty());
                if (result != XZ_APP_OK) return result;
                if (!segments_) continue;
            }
            const size_t length = laces_[index_++];
            if (packet_.size() + length > 8192) return XZ_APP_ERR_LIMIT;
            const size_t start = packet_.size();
            packet_.resize(start + length);
            if (length && !ReadBytes(body_, packet_.data() + start, length)) return XZ_APP_ERR_IO;
            body_ += length;
            if (length < 255) return XZ_APP_OK;
        }
    }

    const std::vector<uint8_t>& Packet() const { return packet_; }
    uint16_t PreSkip() const { return pre_skip_; }
    int16_t Gain() const { return gain_; }
    uint64_t Duration48k() const { return duration_48k_; }
    uint64_t FinalGranule() const { return final_granule_; }

private:
    static uint16_t U16(const uint8_t* p) {
        return uint16_t(p[0]) | uint16_t(p[1]) << 8;
    }

    static uint32_t U32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    static uint64_t U64(const uint8_t* p) {
        return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32;
    }

    static uint32_t Crc(uint32_t crc, const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            crc ^= uint32_t(data[i]) << 24;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
        }
        return crc;
    }

    bool ReadBytes(uint64_t offset, void* buffer, size_t length) {
        return read_ && offset <= size_ && length <= size_ - offset && read_(context_, offset, buffer, length);
    }

    int CheckPage(uint64_t offset, std::array<uint8_t, 27>& header,
                  std::array<uint8_t, 255>& laces, uint64_t& end) {
        if (offset > size_ || size_ - offset < header.size()) return XZ_APP_ERR_INVALID;
        if (!ReadBytes(offset, header.data(), header.size())) return XZ_APP_ERR_IO;
        if (std::memcmp(header.data(), "OggS", 4) || header[4] != 0 || (header[5] & ~7u))
            return XZ_APP_ERR_INVALID;
        if (header[26] > size_ - offset - header.size()) return XZ_APP_ERR_INVALID;
        if (!ReadBytes(offset + header.size(), laces.data(), header[26])) return XZ_APP_ERR_IO;
        size_t payload = 0;
        for (unsigned i = 0; i < header[26]; ++i) payload += laces[i];
        const uint64_t body = offset + header.size() + header[26];
        if (body > size_ || payload > size_ - body) return XZ_APP_ERR_INVALID;
        end = body + payload;
        const uint32_t expected = U32(header.data() + 22);
        std::array<uint8_t, 27> checked = header;
        std::memset(checked.data() + 22, 0, 4);
        uint32_t crc = Crc(0, checked.data(), checked.size());
        crc = Crc(crc, laces.data(), header[26]);
        std::array<uint8_t, 512> chunk;
        for (uint64_t position = body; position < end;) {
            const size_t bytes = static_cast<size_t>(std::min<uint64_t>(chunk.size(), end - position));
            if (!ReadBytes(position, chunk.data(), bytes)) return XZ_APP_ERR_IO;
            crc = Crc(crc, chunk.data(), bytes);
            position += bytes;
        }
        return crc == expected ? XZ_APP_OK : XZ_APP_ERR_INVALID;
    }

    int LoadPage(bool continued) {
        std::array<uint8_t, 27> header;
        uint64_t end = 0;
        const int result = CheckPage(offset_, header, laces_, end);
        if (result != XZ_APP_OK) return result;
        if (bool(header[5] & 1) != continued || U32(header.data() + 18) != sequence_++)
            return XZ_APP_ERR_INVALID;
        if (!serial_known_) {
            if (!(header[5] & 2)) return XZ_APP_ERR_INVALID;
            serial_ = U32(header.data() + 14);
            serial_known_ = true;
        } else if ((header[5] & 2) || U32(header.data() + 14) != serial_) {
            return XZ_APP_ERR_UNSUPPORTED;
        }
        segments_ = header[26];
        index_ = 0;
        body_ = offset_ + header.size() + segments_;
        offset_ = end;
        eos_ = (header[5] & 4) != 0;
        if (eos_) {
            final_granule_ = U64(header.data() + 6);
            if (end != size_ || final_granule_ == UINT64_MAX ||
                !segments_ || laces_[segments_ - 1] == 255) return XZ_APP_ERR_INVALID;
        }
        return XZ_APP_OK;
    }

    int FindDuration() {
        // Ogg 页最大 65307 字节；只检查文件尾部，不扫描整首音乐。
        const uint64_t start = size_ > 65307 ? size_ - 65307 : 0;
        std::array<uint8_t, 515> chunk;
        for (uint64_t offset = start; offset + 4 <= size_;) {
            const size_t length = static_cast<size_t>(std::min<uint64_t>(chunk.size(), size_ - offset));
            if (!ReadBytes(offset, chunk.data(), length)) return XZ_APP_ERR_IO;
            for (size_t i = 0; i + 4 <= length; ++i) {
                if (std::memcmp(chunk.data() + i, "OggS", 4)) continue;
                std::array<uint8_t, 27> header;
                std::array<uint8_t, 255> laces;
                uint64_t end = 0;
                const int result = CheckPage(offset + i, header, laces, end);
                if (result == XZ_APP_ERR_IO) return result;
                if (result != XZ_APP_OK || end != size_ || !(header[5] & 4) ||
                    U32(header.data() + 14) != serial_) continue;
                const uint64_t granule = U64(header.data() + 6);
                if (granule == UINT64_MAX || granule <= pre_skip_) return XZ_APP_ERR_INVALID;
                duration_48k_ = granule - pre_skip_;
                return XZ_APP_OK;
            }
            if (length <= 3) break;
            offset += length - 3;
        }
        return XZ_APP_ERR_INVALID;
    }

    Read read_ = nullptr; // 宿主读取函数，不引用 ELF 代码
    void* context_ = nullptr; // 宿主文件上下文
    uint64_t size_ = 0; // 音频区域字节数
    uint64_t offset_ = 0; // 下一页起始位置
    uint64_t body_ = 0; // 当前页下一段的位置
    uint64_t final_granule_ = UINT64_MAX; // 已读 EOS 页的 48kHz 结束采样位置
    uint64_t duration_48k_ = 0; // 扣除 pre-skip 后的有效采样数
    uint32_t sequence_ = 0; // 下一页序号
    uint32_t serial_ = 0; // 当前逻辑流编号
    uint16_t pre_skip_ = 0; // Opus 头声明的 48kHz 起始裁剪量
    int16_t gain_ = 0; // Opus 输出增益，Q8 dB
    unsigned segments_ = 0; // 当前页段数
    unsigned index_ = 0; // 下一段索引
    bool serial_known_ = false; // 已读取首个逻辑流
    bool eos_ = false; // 当前页是流结束页
    std::array<uint8_t, 255> laces_ {}; // 当前页段表
    std::vector<uint8_t> packet_; // 单个包，限制为 8192 字节
};
