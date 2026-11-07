#pragma once

#include <algorithm>
#include <string>
#include "lvgl.h"
#include "text_encoding.h"
#include "vertical_text.h"

namespace reader {

/** @brief 使用字体包围框累计当前行可安全压缩的空白。 */
inline void ObserveTextGlyph(TextSpacingState& spacing, const lv_font_t* font, uint32_t cp) {
    lv_font_glyph_dsc_t glyph = {};
    const bool known = font && lv_font_get_glyph_dsc(font, &glyph, cp, 0);
    spacing.Observe(std::max(1, static_cast<int>(glyph.adv_w)), glyph.ofs_x, glyph.box_w, known);
}

/** @brief 按当前行可用宽度计算尾标点所需的有限负字距。 */
inline int TextLineLetterSpace(const lv_font_t* font, const std::string& text, int available,
                               int* rendered_width = nullptr, int* gap_count = nullptr) {
    if (rendered_width) *rendered_width = 0;
    if (!font) return 0;
    int width = 0;
    size_t glyphs = 0;
    TextSpacingState spacing;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(text.data()) + i, text.size() - i, &cp);
        if (!n || cp == '\n' || cp == '\r') return 0;
        ObserveTextGlyph(spacing, font, cp);
        width += std::max(1, static_cast<int>(lv_font_get_glyph_width(font, cp, 0)));
        ++glyphs;
        i += n;
    }
    if (gap_count) *gap_count = glyphs > 1 ? static_cast<int>(glyphs - 1) : 0;
    const int letter_space = PunctuationLetterSpace(width, available, glyphs, font->line_height, spacing.limit);
    if (rendered_width) *rendered_width = width + (glyphs > 1 ? letter_space * static_cast<int>(glyphs - 1) : 0);
    return letter_space;
}

struct TextLineSpacing {
    int letter_space = 0; // 字形间固定收紧量
    int extra = 0; // 两端对齐需要分配的额外像素总量
    int gaps = 0; // 可分配的字间隔数

    int Offset(int index) const {
        return index * letter_space + (gaps > 0 ? static_cast<int>(static_cast<int64_t>(extra) * index / gaps) : 0);
    }
};

/** @brief 生成横排或竖排的字距，两端对齐只扩展容量折行的正文。 */
inline TextLineSpacing MakeTextLineSpacing(const lv_font_t* font, const std::string& text,
                                           int available, bool justify, bool vertical = false) {
    TextLineSpacing spacing;
    if (!font) return spacing;
    int width = 0;
    if (!vertical) {
        spacing.letter_space = TextLineLetterSpace(font, text, available, &width, &spacing.gaps);
    } else {
        int glyphs = 0;
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = 0;
            const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(text.data()) + i, text.size() - i, &cp);
            if (!n || cp == '\n' || cp == '\r') return spacing;
            width += VerticalAdvance(cp, font->line_height,
                std::max(1, static_cast<int>(lv_font_get_glyph_width(font, cp, 0))));
            ++glyphs;
            i += n;
        }
        spacing.gaps = std::max(0, glyphs - 1);
    }
    if (justify && spacing.gaps > 0) spacing.extra = std::max(0, available - width);
    return spacing;
}

} // namespace reader
