#include <cstring>
#include <string>
#include "ui/common/widgets/screen_text.h"

lv_coord_t ScreenMeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

std::string ScreenEllipsizeText(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (font == nullptr || max_w <= 0 || ScreenMeasureTextWidth(font, text.c_str()) <= max_w) {
        return text;
    }
    constexpr const char* kEllipsis = "…";
    const lv_coord_t suffix_w = ScreenMeasureTextWidth(font, kEllipsis);
    if (suffix_w >= max_w) {
        return kEllipsis;
    }
    const lv_coord_t budget = max_w - suffix_w;
    std::string out;
    out.reserve(text.size());
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(d + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        if (gw <= 0) {
            gw = 1;
        }
        if (used + gw > budget) {
            break;
        }
        out.append(text, i, n);
        used += gw;
        i += n;
    }
    out.append(kEllipsis);
    return out;
}

std::string ScreenLayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w) {
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    if (font == nullptr || max_w <= 0) {
        return text;
    }

    const uint8_t* d = reinterpret_cast<const uint8_t*>(text);
    const size_t len = std::strlen(text);
    auto glyph_w = [font](uint32_t cp) -> lv_coord_t {
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        return gw > 0 ? gw : 1;
    };

    std::string line1;
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < len) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > max_w) {
            break;
        }
        line1.append(text + i, n);
        used += gw;
        i += n;
    }
    if (i >= len) {
        return line1;
    }

    lv_coord_t rest_w = 0;
    for (size_t j = i; j < len;) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(d + j, len - j, &cp);
        if (n == 0) {
            break;
        }
        rest_w += glyph_w(cp);
        j += n;
    }
    if (rest_w <= max_w) {
        return line1 + "\n" + std::string(text + i);
    }

    constexpr const char* kEllipsis = "…";
    uint32_t ell_cp = 0;
    Utf8Next(reinterpret_cast<const uint8_t*>(kEllipsis), 3, &ell_cp);
    const lv_coord_t budget = max_w - glyph_w(ell_cp);

    std::string line2;
    used = 0;
    while (i < len && budget > 0) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > budget) {
            break;
        }
        line2.append(text + i, n);
        used += gw;
        i += n;
    }
    line2.append(kEllipsis);
    return line1 + "\n" + line2;
}
