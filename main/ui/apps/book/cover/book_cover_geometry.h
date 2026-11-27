#pragma once
#include <algorithm>

constexpr int kCoverFrameBorder = 4;  // Solid cover outline, shared by layout and drawing.

inline int BookCoverCornerRadius(int width, int height) {
    const int maximum = std::min(width, height) / 2 - 2;
    return maximum > 0 ? std::min(std::max(10, width / 6), maximum) : 0;
}

// Left corners are square; only the upper/lower right corners are rounded.
inline bool BookCoverContainsPoint(int x1, int y1, int x2, int y2, int x, int y) {
    if (x < x1 || x > x2 || y < y1 || y > y2) {
        return false;
    }
    const int radius = BookCoverCornerRadius(x2 - x1 + 1, y2 - y1 + 1);
    const int cx = x2 - radius;
    if (!radius || x <= cx) {
        return true;
    }
    const int dy = y < y1 + radius ? y1 + radius - y : (y > y2 - radius ? y - (y2 - radius) : 0);
    const int dx = x - cx;
    return dx * dx + dy * dy <= radius * radius;
}
