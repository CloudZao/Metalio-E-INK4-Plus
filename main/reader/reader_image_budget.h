#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

// 封面旁路权威档：覆盖最近阅读 / 详情大槽 / 目录（显示只缩小、不放大）。
constexpr int kCoverSidecarTargetW = 320;
constexpr int kCoverSidecarTargetH = 428; // 约 3:4

/** @brief 源图不放大能否盖住 need 框（s=min(need/src)≤1 则可只缩小显示） */
inline bool RasterCoversNeed(int src_w, int src_h, int need_w, int need_h) {
    if (src_w <= 0 || src_h <= 0 || need_w <= 0 || need_h <= 0) {
        return false;
    }
    const float sx = static_cast<float>(need_w) / static_cast<float>(src_w);
    const float sy = static_cast<float>(need_h) / static_cast<float>(src_h);
    const float s = sx < sy ? sx : sy;
    return s <= 1.001f;
}

}  // namespace reader
