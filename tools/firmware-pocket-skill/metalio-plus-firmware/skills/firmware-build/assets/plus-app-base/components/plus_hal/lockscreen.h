#pragma once
#include <cstddef>
#include <cstdint>

// Code-drawn lock screens, with no LVGL, heap, font files or image assets.
// Physical framebuffer: 1216 x 684, row-major 4 bpp, EVEN X in LOW nibble,
// 0 = black and 15 = white. Logical drawing is portrait 684 x 1216.
// Unsupported dimensions and null pointers are deliberately left untouched.
namespace plus_lockscreen {

enum class Style { Pocket, Weather, Notepad };

namespace detail {
constexpr int kWidth = 684;
constexpr int kHeight = 1216;
constexpr int kPhysicalWidth = kHeight;
constexpr int kPhysicalHeight = kWidth;

class Canvas {
public:
    explicit Canvas(uint8_t* pixels) : pixels_(pixels) {}

    void Pixel(int x, int y, bool black = true) {
        if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
        const int px = y;
        const int py = kWidth - 1 - x;
        uint8_t& byte = pixels_[std::size_t(py) * (kPhysicalWidth / 2) + px / 2];
        const uint8_t gray = black ? 0 : 15;
        byte = (px & 1) ? uint8_t((byte & 0x0f) | (gray << 4))
                        : uint8_t((byte & 0xf0) | gray);
    }

    void Rect(int x, int y, int w, int h, bool black = true) {
        const int right = x + w < kWidth ? x + w : kWidth;
        const int bottom = y + h < kHeight ? y + h : kHeight;
        for (int row = y < 0 ? 0 : y; row < bottom; ++row)
            for (int col = x < 0 ? 0 : x; col < right; ++col) Pixel(col, row, black);
    }

    void Disk(int cx, int cy, int radius, bool black = true) {
        if (radius < 0) return;
        for (int dy = -radius; dy <= radius; ++dy) {
            int span = radius;
            while (span * span + dy * dy > radius * radius) --span;
            Rect(cx - span, cy + dy, 2 * span + 1, 1, black);
        }
    }

    void Circle(int cx, int cy, int radius, int stroke = 3) {
        Disk(cx, cy, radius);
        if (radius >= stroke) Disk(cx, cy, radius - stroke, false);
    }

    void Line(int x0, int y0, int x1, int y1, int stroke = 2, bool black = true) {
        const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
        const int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            const int half = stroke / 2;
            Rect(x0 - half, y0 - half, stroke, stroke, black);
            if (x0 == x1 && y0 == y1) break;
            const int doubled = 2 * error;
            if (doubled >= dy) { error += dy; x0 += sx; }
            if (doubled <= dx) { error += dx; y0 += sy; }
        }
    }

    void Rounded(int x, int y, int w, int h, int radius, bool black = true) {
        Rect(x + radius, y, w - 2 * radius, h, black);
        Rect(x, y + radius, w, h - 2 * radius, black);
        Disk(x + radius, y + radius, radius, black);
        Disk(x + w - radius - 1, y + radius, radius, black);
        Disk(x + radius, y + h - radius - 1, radius, black);
        Disk(x + w - radius - 1, y + h - radius - 1, radius, black);
    }

    void Frame(int x, int y, int w, int h, int radius, int stroke = 3) {
        Rounded(x, y, w, h, radius);
        Rounded(x + stroke, y + stroke, w - 2 * stroke, h - 2 * stroke,
                radius > stroke ? radius - stroke : 0, false);
    }

    template<std::size_t N> void Polygon(const int (&points)[N][2], bool black = true) {
        int top = points[0][1], bottom = top;
        for (std::size_t i = 1; i < N; ++i) {
            if (points[i][1] < top) top = points[i][1];
            if (points[i][1] > bottom) bottom = points[i][1];
        }
        for (int y = top; y <= bottom; ++y) {
            int left = kWidth, right = -1;
            for (std::size_t i = 0; i < N; ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % N];
                if (a[1] == b[1]) continue;
                const int low = a[1] < b[1] ? a[1] : b[1];
                const int high = a[1] > b[1] ? a[1] : b[1];
                if (y < low || y > high) continue;
                const int x = a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                if (x < left) left = x;
                if (x > right) right = x;
            }
            if (left <= right) Rect(left, y, right - left + 1, 1, black);
        }
    }

    static uint8_t Glyph(char c, int row) {
        // Deliberately small, original 5 x 7 uppercase lettering.
        static constexpr uint8_t letters[26][7] = {
            {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
            {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
            {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
            {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
            {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
            {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
            {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
            {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
            {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
            {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
            {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
            {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
            {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
        };
        if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
        if (c == '-') return row == 3 ? 14 : 0;
        if (c == '/') return uint8_t(1u << (row * 4 / 6));
        return 0;
    }

    void Text(int x, int y, const char* text, int scale = 2, bool black = true) {
        for (; *text; ++text, x += 7 * scale)
            for (int row = 0; row < 7; ++row) {
                const uint8_t bits = Glyph(*text, row);
                for (int col = 0; col < 5; ++col)
                    if (bits & (16u >> col)) Rect(x + col * scale, y + row * scale, scale, scale, black);
            }
    }

    void Centered(int y, const char* text, int scale = 2) {
        int count = 0;
        for (const char* p = text; *p; ++p) ++count;
        Text((kWidth - (count * 7 - 2) * scale) / 2, y, text, scale);
    }

private:
    uint8_t* pixels_;
};

inline void Lock(Canvas& c, int cx, int y) {
    c.Circle(cx, y + 16, 16, 4);
    c.Rounded(cx - 24, y + 15, 49, 38, 6);
    c.Disk(cx, y + 30, 4, false);
    c.Rect(cx - 2, y + 31, 5, 10, false);
}

inline void Pocket(Canvas& c) {
    // Three cards disappear into a single outlined pocket.
    c.Frame(226, 331, 214, 180, 12, 4);
    c.Frame(208, 352, 250, 176, 12, 4);
    c.Frame(190, 376, 286, 225, 14, 4);
    c.Line(218, 407, 324, 407, 3);
    c.Line(218, 425, 400, 425, 3);
    c.Rounded(182, 462, 320, 200, 22, false);
    c.Line(183, 466, 183, 580, 5);
    c.Line(501, 466, 501, 580, 5);
    c.Line(183, 580, 230, 642, 5);
    c.Line(230, 642, 454, 642, 5);
    c.Line(454, 642, 501, 580, 5);
    c.Line(183, 466, 342, 519, 5);
    c.Line(342, 519, 501, 466, 5);
    c.Line(201, 485, 201, 573, 2);
    c.Line(201, 573, 240, 624, 2);
    c.Line(240, 624, 445, 624, 2);
    c.Line(445, 624, 483, 573, 2);
    c.Line(483, 573, 483, 485, 2);
    c.Circle(342, 565, 18, 3);
    c.Line(333, 565, 351, 565, 3);
    c.Line(342, 556, 342, 574, 3);
}

inline void Weather(Canvas& c) {
    const int sx = 286, sy = 428;
    c.Circle(sx, sy, 66, 5);
    // Twelve rays, kept separate from the sun's outline.
    const int rays[][4] = {
        {0,-84,0,-106}, {42,-73,53,-92}, {73,-42,92,-53},
        {84,0,106,0}, {73,42,92,53}, {42,73,53,92},
        {0,84,0,106}, {-42,73,-53,92}, {-73,42,-92,53},
        {-84,0,-106,0}, {-73,-42,-92,-53}, {-42,-73,-53,-92}
    };
    for (const auto& ray : rays) c.Line(sx + ray[0], sy + ray[1], sx + ray[2], sy + ray[3], 4);
    // Filled white cloud masks the lower rays; all final ink is black/white.
    c.Rounded(274, 461, 242, 104, 45);
    c.Disk(347, 459, 61);
    c.Disk(421, 471, 53);
    c.Rounded(279, 466, 232, 94, 40, false);
    c.Disk(347, 459, 56, false);
    c.Disk(421, 471, 48, false);
    c.Rect(303, 480, 184, 57, false);
    c.Line(170, 622, 514, 622, 2);
    c.Line(214, 645, 470, 645, 2);
    c.Line(266, 668, 418, 668, 2);
}

inline void Notepad(Canvas& c) {
    c.Frame(212, 326, 252, 322, 14, 4);
    c.Line(248, 329, 248, 644, 3);
    for (int i = 0; i < 5; ++i) {
        const int y = 370 + i * 49;
        c.Rounded(199, y, 28, 13, 6);
        c.Rounded(202, y + 3, 22, 7, 3, false);
    }
    c.Line(276, 381, 420, 381, 4);
    c.Line(276, 421, 420, 421, 3);
    c.Line(276, 461, 392, 461, 3);
    c.Line(276, 501, 374, 501, 3);
    c.Line(276, 541, 345, 541, 3);
    const int silhouette[][2] = {{365,599},{389,543},{483,378},{513,395},{419,560}};
    const int body[][2] = {{395,543},{484,386},{505,398},{416,554}};
    const int tip[][2] = {{372,588},{391,549},{410,560}};
    c.Polygon(silhouette);
    c.Polygon(body, false);
    c.Polygon(tip, false);
    c.Line(402, 546, 492, 390, 2);
    c.Line(473, 399, 501, 415, 4);
}

} // namespace detail

inline void Draw(uint8_t* physical4bpp, int physical_width, int physical_height, Style style) {
    if (!physical4bpp || physical_width != detail::kPhysicalWidth ||
        physical_height != detail::kPhysicalHeight) return;
    constexpr std::size_t bytes = std::size_t(detail::kPhysicalWidth) * detail::kPhysicalHeight / 2;
    for (std::size_t i = 0; i < bytes; ++i) physical4bpp[i] = 0xff;
    detail::Canvas c(physical4bpp);

    // One quiet layout for all three applications; only the central motif and
    // its name differ. Fine details use >=2 px strokes on the physical panel.
    c.Text(44, 42, "METALIO / PLUS", 2);
    c.Line(44, 80, 640, 80, 2);
    detail::Lock(c, 342, 111);
    c.Centered(187, "LOCKED", 2);
    c.Frame(44, 244, 596, 552, 23, 2);
    c.Rect(319, 242, 47, 5, false);
    c.Rect(319, 793, 47, 5, false);
    switch (style) {
    case Style::Weather:
        detail::Weather(c);
        c.Centered(730, "WEATHER", 3);
        break;
    case Style::Notepad:
        detail::Notepad(c);
        c.Centered(730, "NOTEPAD", 3);
        break;
    case Style::Pocket:
    default:
        detail::Pocket(c);
        c.Centered(730, "FIRMWARE POCKET", 3);
        break;
    }
    c.Centered(860, "TAKE A LITTLE PAUSE", 3);
    c.Centered(912, "YOUR SESSION IS ON HOLD", 2);

    c.Circle(342, 1033, 26, 4);
    c.Rect(331, 1004, 23, 15, false);
    c.Line(342, 1005, 342, 1032, 4);
    c.Centered(1093, "PRESS POWER TO WAKE", 2);
    c.Line(152, 1141, 532, 1141, 2);
    c.Centered(1170, "HOLD POWER TO SHUT DOWN", 2);
}

} // namespace plus_lockscreen
