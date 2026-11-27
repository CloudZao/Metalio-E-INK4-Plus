#pragma once
#include <cstdint>
#include <cstring>
#include "app_api.h"

struct AppAudioWav {
    uint64_t offset = 0; // PCM 起始字节
    uint32_t bytes = 0;  // PCM 字节数
};

static inline uint32_t AppWavU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/** @brief 解析 16kHz 单声道 16 位 PCM WAV，校验 RIFF 块边界并跳过未知块 */
static inline int AppWavOpen(bool (*read)(void*, uint64_t, void*, size_t), void* context, uint64_t length,
                             AppAudioWav& wav) {
    uint8_t header[16];
    wav = {};
    if (length < 12 || !read(context, 0, header, 12)) {
        return XZ_APP_ERR_INVALID;
    }
    if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    const uint64_t end = uint64_t(AppWavU32(header + 4)) + 8;
    if (end > length || end < 12) {
        return XZ_APP_ERR_INVALID;
    }
    bool format = false, data = false;
    uint64_t pos = 12;
    unsigned chunks = 0;
    while (pos < end && ++chunks <= 1024) {
        if (end - pos < 8 || !read(context, pos, header, 8)) {
            return XZ_APP_ERR_INVALID;
        }
        const uint32_t size = AppWavU32(header + 4);
        pos += 8;
        const uint64_t padded = uint64_t(size) + (size & 1);
        if (padded > end - pos) {
            return XZ_APP_ERR_INVALID;
        }
        if (!std::memcmp(header, "fmt ", 4)) {
            if (format || size < 16 || !read(context, pos, header, 16)) {
                return XZ_APP_ERR_INVALID;
            }
            if (header[0] != 1 || header[1] || header[2] != 1 || header[3] || AppWavU32(header + 4) != 16000 ||
                AppWavU32(header + 8) != 32000 || header[12] != 2 || header[13] || header[14] != 16 || header[15]) {
                return XZ_APP_ERR_UNSUPPORTED;
            }
            format = true;
        } else if (!std::memcmp(header, "data", 4)) {
            if (data || !size || (size & 1)) {
                return XZ_APP_ERR_INVALID;
            }
            wav.offset = pos;
            wav.bytes = size;
            data = true;
        }
        pos += padded;
    }
    return pos == end && format && data ? XZ_APP_OK : XZ_APP_ERR_INVALID;
}

/** @brief 根据 PCM 字节数生成固定格式的 WAV 文件头 */
static inline void AppWavHeader(uint8_t* header, uint32_t bytes) {
    const uint8_t base[44] = {'R', 'I', 'F', 'F', 0,  0, 0,   0,   'W', 'A',  'V',  'E', 'f', 'm', 't',
                              ' ', 16,  0,   0,   0,  1, 0,   1,   0,   0x80, 0x3e, 0,   0,   0,   0x7d,
                              0,   0,   2,   0,   16, 0, 'd', 'a', 't', 'a',  0,    0,   0,   0};
    std::memcpy(header, base, sizeof(base));
    const uint32_t sizes[2] = {bytes + 36, bytes};
    for (unsigned n = 0; n < 2; ++n) {
        for (unsigned i = 0; i < 4; ++i) {
            header[(n ? 40 : 4) + i] = sizes[n] >> (8 * i);
        }
    }
}
