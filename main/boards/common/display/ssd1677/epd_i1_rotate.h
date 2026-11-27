#pragma once

#include <cstdint>

// Portrait I1 -> panel I1, px=ly, py=panel_h-1-lx. The caller supplies
// byte-aligned x/w and an in-bounds logical rectangle. Unchanged bits survive.
inline void EpdOverlayPortrait(uint8_t* dst, const uint8_t* src, int panel_w, int panel_h, int x, int y, int w, int h) {
    const int ss = (panel_h + 7) / 8;
    const int ds = panel_w / 8;
    for (int ly = y; ly < y + h;) {
        if ((ly & 7) == 0 && ly + 8 <= y + h) {
            for (int lx = x; lx < x + w; lx += 8) {
                uint8_t rows[8];
                for (int i = 0; i < 8; ++i) {
                    rows[i] = src[(ly + i) * ss + lx / 8];
                }
                for (int bit = 0; bit < 8; ++bit) {
                    uint8_t out = 0;
                    for (int i = 0; i < 8; ++i)
                        out |= ((rows[i] >> (7 - bit)) & 1) << (7 - i);
                    dst[(panel_h - 1 - lx - bit) * ds + ly / 8] = out;
                }
            }
            ly += 8;
        } else {
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (ly & 7));
            for (int lx = x; lx < x + w; lx += 8) {
                const uint8_t in = src[ly * ss + lx / 8];
                for (int bit = 0; bit < 8; ++bit) {
                    auto& out = dst[(panel_h - 1 - lx - bit) * ds + ly / 8];
                    out = static_cast<uint8_t>((out & ~mask) | ((in & (0x80 >> bit)) ? mask : 0));
                }
            }
            ++ly;
        }
    }
}
