#include "wallpaper_thumb_priority.h"

#include "sd_access.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>

namespace {
constexpr uint32_t kReadLimit = 4096; // 单文件实际读取量上限
constexpr uint32_t kPositionLimit = 1024 * 1024; // 允许跳过元数据的文件偏移上限
constexpr unsigned kMarkerLimit = 64; // JPEG 头标记数量上限
constexpr int64_t kProbeTimeUs = 20000; // 限制新增文件操作，已读取的缓存仍可解析
constexpr uint64_t kMinimumCost = 4096; // 避免极小文件忽略固定访问成本

uint64_t Add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
uint64_t Multiply(uint64_t a, uint64_t b) {
    return a && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}
uint32_t Big32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
unsigned Big16(const uint8_t* p) { return unsigned(p[0]) << 8 | p[1]; }
unsigned Little16(const uint8_t* p) { return unsigned(p[1]) << 8 | p[0]; }
bool PngSuffix(const char* path) {
    if (!path) return false;
    const size_t length = std::strlen(path);
    if (length < 4) return false;
    const char* suffix = path + length - 4;
    return suffix[0] == '.' && (suffix[1] == 'p' || suffix[1] == 'P') &&
           (suffix[2] == 'n' || suffix[2] == 'N') && (suffix[3] == 'g' || suffix[3] == 'G');
}

class Probe {
public:
    Probe(FILE* file, size_t bytes, bool (*cancelled)(void*), void* user, int64_t started)
        : file_(file), bytes_(bytes), cancelled_(cancelled), user_(user), started_(started) {}
    bool Cancelled() const { return cancelled_ && cancelled_(user_); }
    bool Read(uint32_t offset, uint8_t* out, size_t size) {
        if (Cancelled() || offset > bytes_ || size > bytes_ - offset ||
            offset > kPositionLimit || size > kPositionLimit - offset) return false;
        if (offset < first_ || uint64_t(offset) + size > uint64_t(first_) + count_) {
            if (Expired() || read_ == kReadLimit || std::fseek(file_, offset, SEEK_SET) != 0 ||
                Cancelled() || Expired()) return false;
            first_ = offset;
            const size_t wanted = std::min<size_t>({sizeof(buffer_), size_t(kReadLimit - read_),
                                           bytes_ - offset, size_t(kPositionLimit - offset)});
            count_ = std::fread(buffer_, 1, wanted, file_);
            read_ += count_;
            if (Cancelled() || count_ < size) return false;
        }
        std::memcpy(out, buffer_ + offset - first_, size);
        return true;
    }
private:
    bool Expired() const { return esp_timer_get_time() - started_ >= kProbeTimeUs; }
    FILE* file_; // 由调用方关闭的探测文件
    size_t bytes_; // 列表收集时的文件长度
    bool (*cancelled_)(void*); // 页面生命周期与批次取消条件
    void* user_; // 取消回调上下文
    int64_t started_; // 包含打开文件时间的探测起点
    uint8_t buffer_[512]{}; // 无堆分配的小型读取缓存
    uint32_t first_ = UINT32_MAX; // 缓存对应的文件偏移
    size_t count_ = 0; // 缓存有效长度
    uint32_t read_ = 0; // 本次实际读取量
};

bool EstimatePng(const uint8_t* head, uint64_t bytes, uint64_t& cost) {
    if (Big32(head + 8) != 13 || std::memcmp(head + 12, "IHDR", 4)) return false;
    const uint32_t width = Big32(head + 16), height = Big32(head + 20);
    const unsigned depth = head[24], color = head[25];
    if (!width || !height || width > INT32_MAX || height > INT32_MAX ||
        head[26] != 0 || head[27] != 0 || head[28] > 1) return false;
    unsigned channels;
    switch (color) {
        case 0: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false; break;
        case 2: channels = 3; if (depth != 8 && depth != 16) return false; break;
        case 3: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return false; break;
        case 4: channels = 2; if (depth != 8 && depth != 16) return false; break;
        case 6: channels = 4; if (depth != 8 && depth != 16) return false; break;
        default: return false;
    }
    const uint64_t row = (uint64_t(width) * channels * depth + 7) / 8 + 1;
    const uint64_t raw = Multiply(row, height);
    cost = raw == UINT64_MAX ? UINT64_MAX : Add(Multiply(bytes, 2), raw);
    return true;
}

bool EstimateJpeg(Probe& probe, size_t bytes, uint64_t& cost) {
    uint32_t position = 2;
    uint8_t marker[8];
    for (unsigned count = 0; count < kMarkerLimit; ++count) {
        if (!probe.Read(position, marker, 2) || marker[0] != 255) return false;
        const unsigned type = marker[1];
        if (type == 255) { ++position; continue; }
        position += 2;
        if (type == 0 || type == 0xd8 || type == 0xd9 || type == 0xda) return false;
        if (type == 1 || (type >= 0xd0 && type <= 0xd7)) continue;
        if (!probe.Read(position, marker, 2)) return false;
        const unsigned length = Big16(marker);
        if (length < 2 || position > bytes || length > bytes - position ||
            length > kPositionLimit - position) return false;
        if (type == 0xc0 || type == 0xc2) {
            if (length < 8 || !probe.Read(position, marker, 8)) return false;
            const unsigned width = Big16(marker + 5), height = Big16(marker + 3), components = marker[7];
            if (marker[2] != 8 || !width || !height || (components != 1 && components != 3) ||
                length != 8 + 3 * components) return false;
            const uint64_t pixels = uint64_t(width) * height;
            cost = type == 0xc2 ? Add(Add(bytes, bytes / 2), pixels / 3) : Add(bytes, pixels / 8);
            return true;
        }
        if (type >= 0xc0 && type <= 0xcf && type != 0xc4 && type != 0xc8 && type != 0xcc) return false;
        position += length;
    }
    return false;
}
}

uint64_t Wallpaper_EstimateThumbCost(const char* path, size_t bytes,
                                     bool (*cancelled)(void*), void* user) {
    const uint64_t fallback = std::max(kMinimumCost, Multiply(bytes, PngSuffix(path) ? 64 : 2));
    const int64_t started = esp_timer_get_time();
    if (!path || !path[0] || !bytes || (cancelled && cancelled(user))) return fallback;
    sd_access::Transaction transaction;
    if (!transaction || (cancelled && cancelled(user)) ||
        esp_timer_get_time() - started >= kProbeTimeUs) return fallback;
    FILE* file = std::fopen(path, "rb");
    if (!file) return fallback;
    setvbuf(file, nullptr, _IONBF, 0);
    Probe probe(file, bytes, cancelled, user, started);
    uint8_t header[29];
    uint64_t cost = fallback;
    bool known = false;
    if (probe.Read(0, header, std::min(bytes, sizeof(header)))) {
        if (bytes >= 8 && (!std::memcmp(header, "A2I1", 4) ||
                          !std::memcmp(header, "A2L8", 4) || !std::memcmp(header, "EBGR", 4))) {
            const bool bits = !std::memcmp(header, "A2I1", 4);
            const unsigned width = Little16(header + 4), height = Little16(header + 6);
            const unsigned stride = bits && bytes >= 20 ? Little16(header + 8) : width;
            const unsigned offset = bits ? 20 : 8;
            known = width && height && (!bits || bytes >= 20) && stride >= (bits ? (width + 7) / 8 : width) &&
                    uint64_t(offset) + uint64_t(stride) * height <= bytes;
            if (known) cost = bytes / 4;
        } else if (bytes >= 33 && !std::memcmp(header, "\x89PNG\r\n\x1a\n", 8)) {
            known = EstimatePng(header, bytes, cost);
        } else if (bytes >= 2 && header[0] == 255 && header[1] == 216) {
            known = EstimateJpeg(probe, bytes, cost);
        }
    }
    if (probe.Cancelled()) known = false;
    if (std::fclose(file) != 0 || probe.Cancelled()) known = false;
    return known ? std::max(kMinimumCost, cost) : fallback;
}
