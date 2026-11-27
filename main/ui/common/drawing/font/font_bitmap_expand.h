#pragma once

#include <array>
#include <cstdint>
#include <cstring>

template <unsigned Bits, unsigned Group>
constexpr auto FontBitmapExpandDetailMakeTable() {
    std::array<std::array<uint8_t, Group>, 1u << (Bits * Group)> table{};
    constexpr unsigned mask = (1u << Bits) - 1;
    for (unsigned value = 0; value < table.size(); ++value) {
        for (unsigned pixel = 0; pixel < Group; ++pixel) {
            table[value][pixel] = ((value >> ((Group - 1 - pixel) * Bits)) & mask) * 255 / mask;
        }
    }
    return table;
}

template <unsigned Bits, unsigned Group>
__attribute__((always_inline)) inline void FontBitmapExpandDetailExpand(const uint8_t* src, uint16_t width,
                                                                        uint16_t height, uint8_t* dst,
                                                                        uint32_t stride) {
    // Immutable tables live in Flash (1600 bytes total); no glyph cache or RAM LUT.
    static constexpr auto table = FontBitmapExpandDetailMakeTable<Bits, Group>();
    constexpr unsigned per_byte = 8 / Bits;
    constexpr unsigned mask = (1u << Bits) - 1;
    uint32_t position = 0;
    for (uint16_t y = 0; y < height; ++y) {
        auto* row = dst + size_t(y) * stride;
        uint32_t x = 0;
        // Packed glyph rows are continuous, not individually byte-aligned.
        while (x < width && position % Group) {
            row[x++] =
                ((src[position / per_byte] >> ((per_byte - 1 - position % per_byte) * Bits)) & mask) * 255 / mask;
            ++position;
        }
        while (x + Group <= width) {
            const unsigned shift = (per_byte - Group - position % per_byte) * Bits;
            const unsigned value = (src[position / per_byte] >> shift) & ((1u << (Bits * Group)) - 1);
            std::memcpy(row + x, table[value].data(), Group);
            x += Group;
            position += Group;
        }
        while (x < width) {
            row[x++] =
                ((src[position / per_byte] >> ((per_byte - 1 - position % per_byte) * Bits)) & mask) * 255 / mask;
            ++position;
        }
    }
}

__attribute__((optimize("O3"))) inline void FontBitmapExpandUnpackToA8(const uint8_t* src, uint8_t bpp, uint16_t width,
                                                                       uint16_t height, uint8_t* dst, uint32_t stride) {
    if (bpp == 1) {
        FontBitmapExpandDetailExpand<1, 4>(src, width, height, dst, stride);
    } else if (bpp == 2) {
        FontBitmapExpandDetailExpand<2, 4>(src, width, height, dst, stride);
    } else {
        FontBitmapExpandDetailExpand<4, 2>(src, width, height, dst, stride);
    }
}
