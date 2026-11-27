#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

inline constexpr int kOpusFrameDurationMs = 60;              // 共享编码帧时长，ms
inline constexpr size_t kMaxAudioPayloadBytes = UINT16_MAX;  // 与协议的 16 位载荷长度兼容

// Opus 最长包为 120 ms；整数毫秒字段只能表达 2.5 ms 的偶数倍。
inline constexpr bool IsValidOpusStreamParameters(int sample_rate, int frame_duration) {
    const bool valid_rate = sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
                            sample_rate == 24000 || sample_rate == 48000;
    return valid_rate && frame_duration >= 5 && frame_duration <= 120 && frame_duration % 5 == 0;
}

struct AudioStreamPacket {
    int sample_rate = 0;           // 采样率，Hz
    int frame_duration = 0;        // 帧时长，ms
    uint32_t timestamp = 0;        // 流时间戳
    std::vector<uint8_t> payload;  // 编码音频数据
};
