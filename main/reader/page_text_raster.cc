/**
 * @file page_text_raster.cc
 * @brief 阅读正文页预渲 L8，供单张 lv_image 走 PARTIAL 上屏（避免逐字 I1 blend）
 */
#include "page_text_raster.h"

#include "reader_page_render.h"
#include "text_encoding.h"
#include "vertical_text.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>

namespace reader {
uint8_t ReadingBackgroundPixel(int mode, int x, int y, int left, int width, int height) {
    if (x < left || x >= left + width || y < 0 || y >= height) return 255;
    x -= left;
    constexpr int pitch = kReadingBackgroundPitch;
    if (mode == 2) {
        const int dot_x = x % pitch, dot_y = y % pitch;
        // I1 output thresholds gray to black/white. A 4x4 checker keeps the dot
        // footprint while reducing ink coverage to 50% for a lighter appearance.
        return (dot_x < 4 && dot_y < 4 && ((dot_x + dot_y) % 2 == 0)) ? 0 : 255;
    }
    if (mode == 1) {
        const int span_x = std::max(1, width - 1), span_y = std::max(1, height - 1);
        const int shorter = std::min(span_x, span_y);
        const int cells = std::max(1, (shorter + pitch / 2) / pitch);
        const int columns = span_x <= span_y ? cells : (span_x * cells + shorter / 2) / shorter;
        const int rows = span_y <= span_x ? cells : (span_y * cells + shorter / 2) / shorter;
        // 均分固定内容区，边线位于两端；相邻格的取整误差不超过 1 像素。
        const bool vertical = x * columns % span_x < columns;
        const bool horizontal = y * rows % span_y < rows;
        const bool corner = (x == 0 || x == width - 1) && (y == 0 || y == height - 1);
        return ((vertical && y % 3 == 0) || (horizontal && x % 3 == 0) || corner) ? 0 : 255;
    }
    return 255;
}

namespace {

constexpr int kGlyphScratch = 96;

uint8_t* GlyphScratch() {
    static uint8_t* s = nullptr;
    if (s == nullptr) {
        s = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(kGlyphScratch) * kGlyphScratch,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s == nullptr) {
            s = static_cast<uint8_t*>(
                heap_caps_malloc(static_cast<size_t>(kGlyphScratch) * kGlyphScratch, MALLOC_CAP_8BIT));
        }
    }
    return s;
}

void PlotInk(RasterImage& img, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height)) {
        return;
    }
    img.pixels[static_cast<size_t>(y) * img.width + static_cast<size_t>(x)] = 0x00;
}

void BlitGlyphA8(RasterImage& img, int dst_x, int dst_y, const uint8_t* a8, uint16_t box_w,
                 uint16_t box_h, uint32_t stride, bool antialias, bool rotate = false,
                 int rotated_height = 0) {
    const int output_w = rotate && rotated_height > 0 ? rotated_height : box_w;
    for (uint16_t gy = 0; gy < box_h; ++gy) {
        const uint8_t* row = a8 + static_cast<size_t>(gy) * stride;
        for (int gx = 0; gx < output_w; ++gx) {
            const uint8_t coverage = row[rotate && rotated_height > 0 ? gx * box_w / output_w : gx];
            // Preserve coverage in the actual pre-rasterized reading path.
            if (coverage != 0) {
                const int x = dst_x + (rotate ? box_h - 1 - gy : gx);
                const int y = dst_y + (rotate ? gx : gy);
                if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
                auto& pixel = img.pixels[static_cast<size_t>(y) * img.width + x];
                pixel = antialias ? (unsigned(pixel) * (255 - coverage) + 127) / 255 : 0;
            }
        }
    }
}

// Apply the former display-side text darkening once, after overlapping glyphs
// have blended. Images and UI transparency must never pass through this policy.
void FinishTextRaster(RasterImage& image, bool antialias) {
    if (antialias) {
        for (auto& pixel : image.pixels) {
            if (pixel != 255) pixel -= pixel / 4;
        }
    }
    image.BindDsc();
}

void DrawUnderline(RasterImage& img, int x0, int x1, int y, int mode) {
    if (mode <= 0 || x1 < x0) {
        return;
    }
    for (int dy = 0; dy < kReaderUnderlineThickness; ++dy) {
        const int py = y + dy;
        for (int x = x0; x <= x1; ++x) {
            if (mode == 2 && ((x - x0) % 10) >= 6) {
                continue;
            }
            PlotInk(img, x, py);
        }
    }
}

lv_coord_t MeasureTextW(const lv_font_t* font, const char* utf8) {
    if (font == nullptr || utf8 == nullptr) {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, utf8, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

bool DrawUtf8Line(RasterImage& img, const lv_font_t* font, int x, int line_top, const char* utf8,
                  bool faux_bold, int* out_end_x, bool vertical = false, bool antialias = false,
                  int* out_ink_bottom = nullptr) {
    if (font == nullptr || utf8 == nullptr) {
        return false;
    }
    uint8_t* scratch = GlyphScratch();
    if (scratch == nullptr) {
        return false;
    }
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, kGlyphScratch, kGlyphScratch, LV_COLOR_FORMAT_A8, kGlyphScratch,
                     scratch, static_cast<size_t>(kGlyphScratch) * kGlyphScratch);

    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
    size_t left = std::strlen(utf8);
    int pen_x = x;
    int ink_bottom = -1;

    while (left > 0) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(p, left, &cp);
        if (n == 0) {
            break;
        }
        p += n;
        left -= n;
        if (cp == '\n' || cp == '\r') {
            if (vertical && cp == '\n') pen_x += font->line_height;
            continue;
        }

        const uint32_t glyph_cp = vertical ? VerticalGlyph(cp) : cp;
        lv_font_glyph_dsc_t gdsc;
        if (!lv_font_get_glyph_dsc(font, &gdsc, glyph_cp, 0)) {
            pen_x += vertical ? VerticalAdvance(cp, font->line_height)
                              : std::max(1, static_cast<int>(font->line_height / 3));
            continue;
        }
        if (gdsc.resolved_font == nullptr) {
            gdsc.resolved_font = font;
        }
        // Use glyph-sized geometry for both built-in and external font callbacks.
        const uint32_t glyph_stride = lv_draw_buf_width_to_stride(gdsc.box_w, LV_COLOR_FORMAT_A8);
        if (static_cast<size_t>(glyph_stride) * gdsc.box_h > draw_buf.data_size) {
            return false;
        }
        draw_buf.header.w = gdsc.box_w;
        draw_buf.header.h = gdsc.box_h;
        draw_buf.header.stride = glyph_stride;
        draw_buf.header.cf = LV_COLOR_FORMAT_A8;
        const void* bm = lv_font_get_glyph_bitmap(&gdsc, &draw_buf);
        if (bm != nullptr && gdsc.box_w > 0 && gdsc.box_h > 0) {
            if (!vertical) ink_bottom = std::max(ink_bottom, ReaderGlyphInkBottom(font, gdsc));
            const auto* db = static_cast<const lv_draw_buf_t*>(bm);
            const uint8_t* a8 = (db != nullptr && db->data != nullptr)
                                    ? static_cast<const uint8_t*>(db->data)
                                    : scratch;
            // Font callbacks may reshape the output buffer (including built-in fonts).
            const uint32_t stride = db->header.stride;
            const bool rotate = vertical && VerticalRotate(glyph_cp);
            // 破折号沿列填满字格，保证相邻两段连接，复用原字形的线宽与覆盖度。
            const int rotated_height = vertical && cp == 0x2014 ? font->line_height : 0;
            const int box_w = rotate ? gdsc.box_h : gdsc.box_w;
            const int box_h = rotated_height > 0 ? rotated_height : rotate ? gdsc.box_w : gdsc.box_h;
            int gx = vertical ? x + (font->line_height - box_w) / 2 : pen_x + gdsc.ofs_x;
            // 与 lv_draw_label：y1 = line_top + (lh - base) - box_h - ofs_y
            int gy = vertical ? line_top + pen_x - x + (font->line_height - box_h) / 2
                                    : line_top + static_cast<int>(font->line_height - font->base_line) -
                                          static_cast<int>(gdsc.box_h) - gdsc.ofs_y;
            if (vertical) {
                const int cell = font->line_height;
                const int top = line_top + pen_x - x;
                switch (cp) {
                    case ',': case '.': case 0xFF0C: case 0x3001: case 0x3002:
                        gx = x + cell - box_w;
                        gy = top;
                        break;
                    case ':': case ';': case '!': case '?':
                    case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:
                        gx = x + cell - box_w;
                        break;
                    default: break;
                }
            }
            BlitGlyphA8(img, gx, gy, a8, gdsc.box_w, gdsc.box_h, stride, antialias, rotate, rotated_height);
            if (faux_bold) {
                BlitGlyphA8(img, gx + 1, gy, a8, gdsc.box_w, gdsc.box_h, stride, antialias, rotate, rotated_height);
            }
            if (font->release_glyph != nullptr) {
                font->release_glyph(font, &gdsc);
            }
        }
        pen_x += vertical ? VerticalAdvance(cp, font->line_height) : static_cast<int>(gdsc.adv_w);
    }
    if (out_end_x != nullptr) {
        *out_end_x = pen_x;
    }
    if (out_ink_bottom != nullptr) *out_ink_bottom = ink_bottom;
    return true;
}

}  // namespace

bool PageIsTextOnly(const Page& page) {
    for (const auto& item : page.items) {
        if (item.kind == ContentKind::kImage) {
            return false;
        }
    }
    return true;
}

int EstimatePageTextHeight(const Page& page, const lv_font_t* font, int spacing_percent,
                           lv_coord_t para_gap, int max_h) {
    if (!font || max_h <= 0) return 0;
    const auto layout = MakeTextLineLayout(font, max_h, spacing_percent);
    if (layout.count <= 0) return 0;
    int row = 0, y = 0;
    bool any = false, previous_title = false;
    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) continue;
        if (item.para_gap_before && any) y += ReaderParagraphGap(para_gap, previous_title);
        int lines = 1;
        if (item.chapter_title) for (size_t i = 0; i + 1 < item.text.size(); ++i) if (item.text[i] == '\n') ++lines;
        while (lines-- > 0) {
            if (row >= layout.count || y + layout.CellHeight(row) > max_h) return 0;
            y += layout.CellHeight(row++);
        }
        previous_title = item.chapter_title;
        any = true;
    }
    return any ? y : 0;
}

bool RasterPageTextToL8(const Page& page, const lv_font_t* font, int spacing_percent,
                        lv_coord_t para_gap, int page_w, int page_h, int underline_mode,
                        RasterImage& out, bool antialias) {
    out.Reset();
    if (font == nullptr || page_w <= 0 || page_h <= 0 || !PageIsTextOnly(page)) {
        return false;
    }
    const auto layout = MakeTextLineLayout(font, page_h, spacing_percent);
    const int image_h = EstimatePageTextHeight(page, font, spacing_percent, para_gap, page_h);
    if (image_h <= 0) return false;
    const size_t n = static_cast<size_t>(page_w) * static_cast<size_t>(image_h);
    try {
        out.pixels.assign(n, 0xFF);
    } catch (...) {
        out.Reset();
        return false;
    }
    if (out.pixels.size() != n) {
        out.Reset();
        return false;
    }
    out.width = static_cast<uint16_t>(page_w);
    out.height = static_cast<uint16_t>(image_h);

    const lv_coord_t indent_px = ParaIndentPadPx(font);
    lv_coord_t y = 0;
    int row = 0;
    bool any = false, previous_title = false;

    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) {
            continue;
        }
        if (item.para_gap_before && any) {
            y = static_cast<lv_coord_t>(y + ReaderParagraphGap(para_gap, previous_title));
        }
        if (row >= layout.count || y + layout.CellHeight(row) > page_h) return false;

        lv_coord_t text_x = 0;
        const char* txt = item.text.c_str();
        // 章名可能含「第N章\\n章节名」：按行画并垂直推进
        if (item.chapter_title && std::strchr(txt, '\n') != nullptr) {
            const char* p = txt;
            int end_x = 0;
            while (p != nullptr && *p != '\0') {
                const char* nl = std::strchr(p, '\n');
                std::string line = nl ? std::string(p, static_cast<size_t>(nl - p)) : std::string(p);
                const lv_coord_t tw = MeasureTextW(font, line.c_str());
                text_x = tw < page_w ? static_cast<lv_coord_t>((page_w - tw) / 2) : 0;
                if (row >= layout.count || y + layout.CellHeight(row) > page_h) return false;
                const int line_y = y + layout.TextOffset(row);
                int ink_bottom = -1;
                if (!DrawUtf8Line(out, font, text_x, line_y, line.c_str(), true,
                                  &end_x, false, antialias, &ink_bottom)) {
                    out.Reset();
                    return false;
                }
                if (underline_mode > 0 && ink_bottom >= 0) {
                    const int ul_y = line_y + ReaderUnderlineTop(ink_bottom);
                    DrawUnderline(out, text_x, end_x - 1, ul_y, underline_mode);
                }
                y += layout.CellHeight(row++);
                p = nl ? nl + 1 : nullptr;
            }
            any = true;
            previous_title = true;
            continue;
        }
        if (item.chapter_title) {
            const lv_coord_t tw = MeasureTextW(font, txt);
            text_x = tw < page_w ? static_cast<lv_coord_t>((page_w - tw) / 2) : 0;
        } else if (item.para_indent) {
            text_x = indent_px;
        }

        int end_x = text_x;
        int ink_bottom = -1;
        const int line_y = y + layout.TextOffset(row);
        if (!DrawUtf8Line(out, font, text_x, line_y, txt, item.chapter_title, &end_x, false, antialias, &ink_bottom)) {
            out.Reset();
            return false;
        }
        if (underline_mode > 0 && ink_bottom >= 0) {
            const int ul_y = line_y + ReaderUnderlineTop(ink_bottom);
            DrawUnderline(out, text_x, end_x - 1, ul_y, underline_mode);
        }
        y += layout.CellHeight(row++);
        any = true;
        previous_title = item.chapter_title;
    }

    FinishTextRaster(out, antialias);
    return any;
}

bool RasterPageVerticalTextToL8(const Page& page, const lv_font_t* font, int spacing_percent,
                                lv_coord_t para_gap, int page_w, int page_h, int rule_mode,
                                RasterImage& out, bool antialias, bool right_to_left) {
    out.Reset();
    if (!font || font->line_height == 0 || page_w <= 0 || page_h <= 0 || !PageIsTextOnly(page)) return false;
    try {
        out.pixels.assign(static_cast<size_t>(page_w) * page_h, 0xFF);
    } catch (...) {
        return false;
    }
    out.width = static_cast<uint16_t>(page_w);
    out.height = static_cast<uint16_t>(page_h);
    const int cell = font->line_height;
    const auto layout = MakeTextLineLayout(font, page_w, spacing_percent, true);
    int x = 0, row = 0;
    bool any = false, previous_title = false;
    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) continue;
        if (any && item.para_gap_before) {
            x += ReaderParagraphGap(para_gap, previous_title);
        }
        if (row >= layout.count || x + layout.CellHeight(row) > page_w) { out.Reset(); return false; }
        const int column_x = (right_to_left ? page_w - x - layout.CellHeight(row) : x) + layout.TextOffset(row);
        const int top = item.para_indent && !item.chapter_title
            ? std::min(2 * cell, std::max(0, page_h - cell)) : 0;
        if (!DrawUtf8Line(out, font, column_x, top, item.text.c_str(), item.chapter_title, nullptr, true, antialias)) {
            out.Reset(); return false;
        }
        if (rule_mode > 0) {
            const int rule_x = column_x + cell;
            for (int y = 0; y < page_h; ++y) {
                if (rule_mode != 2 || y % 10 < 6) PlotInk(out, rule_x, y);
            }
        }
        x += layout.CellHeight(row++);
        previous_title = item.chapter_title;
        any = true;
    }
    FinishTextRaster(out, antialias);
    return any;
}

}  // namespace reader
