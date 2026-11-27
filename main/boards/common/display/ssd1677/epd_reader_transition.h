#pragma once
#include <cstdint>
#include <cstring>

// 方向值遵循 display_orient.h，在完整逻辑面板内变换坐标。
// 面板白边由输出驱动保留。
struct EpdPagePoint {
    int x, y, width, height;  // 逻辑坐标及当前方向对应的逻辑宽高
};
inline EpdPagePoint EpdPageLogicalPoint(int px, int py, int width, int height, int orient) {
    switch (orient) {
        case 1:
            return {px, py, width, height};
        case 2:
            return {width - 1 - px, height - 1 - py, width, height};
        case 3:
            return {py, width - 1 - px, height, width};
        default:
            return {height - 1 - py, px, height, width};
    }
}
struct EpdPanelPoint {
    int x, y;  // 面板像素坐标
};
inline EpdPanelPoint EpdPagePanelPoint(int x, int y, int width, int height, int orient) {
    switch (orient) {
        case 1:
            return {x, y};
        case 2:
            return {width - 1 - x, height - 1 - y};
        case 3:
            return {width - 1 - y, x};
        default:
            return {y, height - 1 - x};
    }
}
constexpr int kEpdRippleSteps = 1;
constexpr int kEpdRippleOne = 4096;
struct EpdRippleFrame {
    int width, height, center, half_width, direction;  // 逻辑尺寸、透镜中心、半宽及翻页方向
};
struct EpdRippleColumn {
    int source_x, inverse_y;  // 源逻辑列及纵向定点采样系数
    bool new_page;            // 当前列是否从目标页采样
};
inline int EpdRippleRound(int value) {
    return value >= 0 ? (value + kEpdRippleOne / 2) / kEpdRippleOne : -((-value + kEpdRippleOne / 2) / kEpdRippleOne);
}
// 竖向透镜在下一页时从右向左移动，上一页时从左向右移动，后方显示目标页。
inline EpdRippleFrame EpdMakeRippleFrame(int width, int height, int direction, int step = 1) {
    step = step < 1 ? 1 : (step > kEpdRippleSteps ? kEpdRippleSteps : step);
    const int travel = (width - 1) * step / (kEpdRippleSteps + 1);
    int half = width / 6;
    half = half < 72 ? 72 : (half > 144 ? 144 : half);
    return {width, height, direction > 0 ? width - 1 - travel : travel, half, direction};
}
// 按逻辑列预计算采样系数，像素循环内复用结果。
inline EpdRippleColumn EpdMakeRippleColumn(const EpdRippleFrame& wave, int x) {
    const int dx = x - wave.center;
    const int distance = dx < 0 ? -dx : dx;
    int weight = 0;
    if (distance < wave.half_width) {
        const int t = distance * kEpdRippleOne / wave.half_width;
        const int bell = kEpdRippleOne - t * t / kEpdRippleOne;
        weight = bell * bell / kEpdRippleOne;
    }
    const int inverse_x = kEpdRippleOne * kEpdRippleOne / (kEpdRippleOne + weight / 8);
    const int inverse_y = kEpdRippleOne * kEpdRippleOne / (kEpdRippleOne + weight / 16);
    int sx = wave.center + EpdRippleRound(dx * inverse_x);
    sx = sx < 0 ? 0 : (sx >= wave.width ? wave.width - 1 : sx);
    return {sx, inverse_y, wave.direction > 0 ? x >= wave.center : x <= wave.center};
}
inline int EpdRippleSampleY(const EpdRippleFrame& wave, const EpdRippleColumn& column, int y) {
    return wave.height / 2 + EpdRippleRound((y - wave.height / 2) * column.inverse_y);
}
// SSD1677 使用高位在前的 I1 压缩像素。基于固定旧帧生成透镜过渡帧，
// 正文区域以外保留目标帧像素。
inline void EpdBuildReaderRippleI1(uint8_t* out, const uint8_t* old_frame, const uint8_t* target, int panel_w,
                                   int panel_h, int orient, int direction, int left, int top, int width, int height) {
    const int stride = panel_w / 8;
    memcpy(out, target, static_cast<size_t>(stride) * panel_h);
    const auto wave = EpdMakeRippleFrame(width, height, direction);
    for (int x = 0; x < width; ++x) {
        const auto column = EpdMakeRippleColumn(wave, x);
        const uint8_t* source = column.new_page ? target : old_frame;
        for (int y = 0; y < height; ++y) {
            const auto dst = EpdPagePanelPoint(left + x, top + y, panel_w, panel_h, orient);
            const int sy = EpdRippleSampleY(wave, column, y);
            const auto src = EpdPagePanelPoint(left + column.source_x, top + sy, panel_w, panel_h, orient);
            if (dst.x < 0 || dst.x >= panel_w || dst.y < 0 || dst.y >= panel_h || src.x < 0 || src.x >= panel_w ||
                src.y < 0 || src.y >= panel_h) {
                continue;
            }
            const bool white = (source[src.y * stride + src.x / 8] & (0x80 >> (src.x & 7))) != 0;
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (dst.x & 7));
            auto& byte = out[dst.y * stride + dst.x / 8];
            if (white) {
                byte |= mask;
            } else {
                byte &= static_cast<uint8_t>(~mask);
            }
        }
    }
}

/** @brief 为 SPI I1 面板构建定向推白过渡帧，正文外像素保持目标帧内容 */
inline void EpdBuildReaderWipeI1(uint8_t* out, const uint8_t* target, int panel_w, int panel_h, int orient,
                                 int direction, int left, int top, int width, int height) {
    const int stride = panel_w / 8;
    memcpy(out, target, static_cast<size_t>(stride) * panel_h);
    for (int x = 0; x < width; ++x) {
        if ((direction > 0 && x >= width / 2) || (direction < 0 && x < width / 2)) {
            continue;
        }
        for (int y = 0; y < height; ++y) {
            const auto point = EpdPagePanelPoint(left + x, top + y, panel_w, panel_h, orient);
            if (point.x >= 0 && point.x < panel_w && point.y >= 0 && point.y < panel_h) {
                out[point.y * stride + point.x / 8] |= static_cast<uint8_t>(0x80 >> (point.x & 7));
            }
        }
    }
}
