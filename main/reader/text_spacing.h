#pragma once

#include <algorithm>
#include <string>
#include "lvgl.h"
#include "text_encoding.h"
#include "vertical_text.h"

namespace reader {

inline bool CompactTextPunctuation(uint32_t cp) {
    return cp == ',' || cp == '.' || cp == 0xFF0C || cp == 0x3001 || cp == 0x3002;
}

inline int TextAdvanceWithGap(int advance, int letter_gap) {
    return std::max(1, advance + letter_gap);
}

inline int TextGlyphAdvance(const lv_font_glyph_dsc_t& glyph) {
    return std::max(1, static_cast<int>(glyph.adv_w));
}

/** @brief 取得行尾小标点可让出的尾部空白，保留字形及 2px 右侧余量。 */
inline int TextTailBlank(uint32_t cp, const lv_font_glyph_dsc_t& glyph) {
    return CompactTextPunctuation(cp) && glyph.box_w > 0
        ? std::max(0, static_cast<int>(glyph.adv_w) - glyph.ofs_x - glyph.box_w - 2) : 0;
}

inline void ObserveTextGlyph(TextSpacingState& spacing, const lv_font_t* font, uint32_t cp,
                              const lv_font_glyph_dsc_t& glyph, bool known = true, int letter_gap = 0) {
    spacing.Observe(TextAdvanceWithGap(TextGlyphAdvance(glyph), letter_gap), glyph.ofs_x, glyph.box_w, known,
        std::max(1, static_cast<int>(font->line_height) / 12), !VerticalClosing(cp),
        known ? std::max(0, TextTailBlank(cp, glyph) + letter_gap) : 0);
}

/** @brief 按实际字形边界累计每处可收紧空白，闭合标点前不分配对齐空白。 */
inline void ObserveTextGlyph(TextSpacingState& spacing, const lv_font_t* font, uint32_t cp, int letter_gap = 0) {
    if (!font) { spacing.capacity = 0; spacing.known = false; return; }
    lv_font_glyph_dsc_t glyph{};
    const bool known = lv_font_get_glyph_dsc(font, &glyph, cp, 0);
    ObserveTextGlyph(spacing, font, cp, glyph, known, letter_gap);
}

struct TextLineSpacing {
    int width = 0; // 最终行宽，含压缩及两端对齐
    int trim = 0; // 仅在溢出时收回行尾小标点的尾部空白
    int shrink = 0; // 当前行需要收紧的像素总数
    int capacity = 0; // 可安全收紧的像素总数
    int extra = 0; // 两端对齐需要分配的额外像素总数
    int gaps = 0; // 可分配对齐空白的间隔数

    int Offset(int slot, int used_capacity = 0) const {
        return (capacity > 0 ? -static_cast<int>(static_cast<int64_t>(shrink) * used_capacity / capacity) : 0) +
            (gaps > 0 ? static_cast<int>(static_cast<int64_t>(extra) * slot / gaps) : 0);
    }
};

/** @brief 生成分页、位图及标签共用的字距；仅收紧真实空白，不缩放字形。 */
inline TextLineSpacing MakeTextLineSpacing(const lv_font_t* font, const std::string& text,
                                           int available, bool justify, bool vertical = false, int letter_gap = 0) {
    TextLineSpacing spacing;
    if (!font) return spacing;
    TextSpacingState state;
    int glyphs = 0;
    uint32_t last_cp = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(text.data()) + i, text.size() - i, &cp);
        if (!n || cp == '\n' || cp == '\r') return spacing;
        if (vertical) {
            spacing.width += TextAdvanceWithGap(VerticalAdvance(cp, font->line_height,
                std::max(1, static_cast<int>(lv_font_get_glyph_width(font, cp, 0)))), letter_gap);
        } else {
            lv_font_glyph_dsc_t glyph{};
            const bool known = lv_font_get_glyph_dsc(font, &glyph, cp, 0);
            ObserveTextGlyph(state, font, cp, glyph, known, letter_gap);
            spacing.width += TextAdvanceWithGap(TextGlyphAdvance(glyph), letter_gap);
        }
        ++glyphs;
        last_cp = cp;
        i += n;
    }
    spacing.gaps = vertical ? std::max(0, glyphs - 1) : state.slots;
    spacing.capacity = vertical ? 0 : state.capacity;
    const int need = std::max(0, spacing.width - available);
    if (!vertical && CanFitTrailingPunctuation(last_cp, spacing.width, available, state)) {
        spacing.trim = std::min(need, state.tail_blank);
        spacing.shrink = need - spacing.trim;
    }
    spacing.width -= spacing.trim + spacing.shrink;
    if (justify && spacing.gaps > 0) spacing.extra = std::max(0, available - spacing.width);
    spacing.width += spacing.extra;
    return spacing;
}

} // namespace reader
