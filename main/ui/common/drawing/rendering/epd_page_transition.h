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
constexpr int kEpdRippleSteps = 2;
constexpr int kEpdRippleBudgetUs = 350000;
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
struct EpdPageRect {
    int left, top, right, bottom;  // 包含边界像素的面板矩形
};
inline EpdPageRect EpdPageColumnRect(int first, int end, int left, int top, int height, int panel_width,
                                     int panel_height, int orient) {
    const auto a = EpdPagePanelPoint(left + first, top, panel_width, panel_height, orient);
    const auto b = EpdPagePanelPoint(left + end - 1, top + height - 1, panel_width, panel_height, orient);
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y};
}

// 按压缩字节复制未变化的列，边缘保留相邻半字节。
inline void EpdPageCopyColumns(uint8_t* dst, const uint8_t* src, int first, int end, int left, int top, int height,
                               int panel_width, int panel_height, int orient) {
    if (first >= end) {
        return;
    }
    const auto rect = EpdPageColumnRect(first, end, left, top, height, panel_width, panel_height, orient);
    const int stride = panel_width / 2;
    for (int y = rect.top; y <= rect.bottom; ++y) {
        int x = rect.left;
        uint8_t* out = dst + y * stride;
        const uint8_t* in = src + y * stride;
        if (x & 1) {
            out[x / 2] = (out[x / 2] & 15) | (in[x / 2] & 240);
            ++x;
        }
        const int bytes = (rect.right + 1 - x) / 2;
        memcpy(out + x / 2, in + x / 2, bytes);
        x += bytes * 2;
        if (x <= rect.right) {
            out[x / 2] = (out[x / 2] & 240) | (in[x / 2] & 15);
        }
    }
}

// 沿用二值过渡的移动透镜，同时保留原始灰度半字节。
// 按逻辑列分块处理，限制竖屏下源和目标的访问范围。
inline __attribute__((optimize("O3"))) void EpdComposeRippleFrame(uint8_t* dst, const uint8_t* old,
                                                                  const uint8_t* target, EpdRippleColumn* columns,
                                                                  const EpdRippleFrame& wave, int left, int top,
                                                                  int panel_width, int panel_height, int orient) {
    memcpy(dst, target, size_t(panel_width / 2) * panel_height);
    EpdPageCopyColumns(dst, old, wave.direction > 0 ? 0 : wave.center + 1,
                       wave.direction > 0 ? wave.center : wave.width, left, top, wave.height, panel_width, panel_height,
                       orient);
    const int first = wave.center - wave.half_width + 1 > 0 ? wave.center - wave.half_width + 1 : 0;
    const int end = wave.center + wave.half_width < wave.width ? wave.center + wave.half_width : wave.width;
    for (int x = first; x < end; ++x) {
        columns[x] = EpdMakeRippleColumn(wave, x);
    }
    const auto p0 = EpdPagePanelPoint(0, 0, panel_width, panel_height, orient);
    const auto p1 = EpdPagePanelPoint(0, 1, panel_width, panel_height, orient);
    const int dx = p1.x - p0.x, dy = p1.y - p0.y, stride = panel_width / 2;
    for (int ty = 0; ty < wave.height; ty += 16) {
        for (int tx = first; tx < end; tx += 16) {
            const int xe = tx + 16 < end ? tx + 16 : end, ye = ty + 16 < wave.height ? ty + 16 : wave.height;
            for (int x = tx; x < xe; ++x) {
                const auto& column = columns[x];
                const uint8_t* src = column.new_page ? target : old;
                const auto start = EpdPagePanelPoint(x + left, top + ty, panel_width, panel_height, orient);
                const auto origin = EpdPagePanelPoint(column.source_x + left, top, panel_width, panel_height, orient);
                int px = start.x, py = start.y;
                for (int y = ty; y < ye; ++y, px += dx, py += dy) {
                    const int sy = EpdRippleSampleY(wave, column, y);
                    const int sx = origin.x + sy * dx, sy_panel = origin.y + sy * dy;
                    const unsigned pixel = (src[sy_panel * stride + sx / 2] >> ((sx & 1) * 4)) & 15;
                    auto& byte = dst[py * stride + px / 2];
                    byte = (px & 1) ? (byte & 15) | (pixel << 4) : (byte & 240) | pixel;
                }
            }
        }
    }
}

inline __attribute__((optimize("O3"))) void EpdComposePageFrame(uint8_t* dst, const uint8_t* old, const uint8_t* target,
                                                                int width, int height, int orient, int direction,
                                                                int step) {
    memcpy(dst, old, size_t(width / 2) * height);
    const auto logical = EpdPageLogicalPoint(0, 0, width, height, orient);
    const int first = direction > 0 ? 0 : logical.width * (3 - step) / 3;
    const int end = direction > 0 ? logical.width * step / 3 : logical.width;
    EpdPageCopyColumns(dst, target, first, end, 0, 0, logical.height, width, height, orient);
}
