/**
 * @file page_text_raster.cc
 * @brief 阅读正文页预渲 L8，供单张 lv_image 走 PARTIAL 上屏（避免逐字 I1 blend）
 */
#include "page_text_raster.h"

#include "reader_page_render.h"
#include "vertical_text.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace reader {
namespace {

constexpr const char* TAG = "PageRaster";
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

size_t Utf8Next(const uint8_t* s, size_t len, uint32_t* cp) {
    if (len == 0) {
        return 0;
    }
    const uint8_t c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *cp = (static_cast<uint32_t>(c & 0x0F) << 12) | (static_cast<uint32_t>(s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *cp = (static_cast<uint32_t>(c & 0x07) << 18) | (static_cast<uint32_t>(s[1] & 0x3F) << 12) |
              (static_cast<uint32_t>(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
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
    // 线宽 2px（相对原先 1px 加粗 1）
    for (int dy = 0; dy < 2; ++dy) {
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
                  bool faux_bold, int* out_end_x, bool vertical = false, bool antialias = false) {
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

int EstimatePageTextHeight(const Page& page, const lv_font_t* font, lv_coord_t line_gap,
                           lv_coord_t para_gap, int max_h) {
    if (font == nullptr || max_h <= 0) {
        return 0;
    }
    const lv_coord_t line_h = font->line_height > 0 ? font->line_height : 24;
    lv_coord_t y = 0;
    bool any = false;
    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) {
            continue;
        }
        if (item.para_gap_before && any) {
            const lv_coord_t extra =
                (para_gap > line_gap) ? static_cast<lv_coord_t>(para_gap - line_gap) : 0;
            y = static_cast<lv_coord_t>(y + extra);
        }
        const char* txt = item.text.c_str();
        if (item.chapter_title && std::strchr(txt, '\n') != nullptr) {
            const char* p = txt;
            while (p != nullptr && *p != '\0') {
                if (y + line_h > max_h) {
                    return max_h;
                }
                y = static_cast<lv_coord_t>(y + line_h + line_gap);
                const char* nl = std::strchr(p, '\n');
                p = nl ? nl + 1 : nullptr;
            }
            any = true;
            continue;
        }
        if (y + line_h > max_h) {
            return max_h;
        }
        y = static_cast<lv_coord_t>(y + line_h + line_gap);
        any = true;
    }
    if (!any) {
        return static_cast<int>(line_h);
    }
    // 末行后多加的 line_gap 去掉，避免多一块空白被 LVGL blend
    if (y > line_gap) {
        y = static_cast<lv_coord_t>(y - line_gap);
    }
    if (y < line_h) {
        y = line_h;
    }
    if (y > max_h) {
        y = static_cast<lv_coord_t>(max_h);
    }
    return static_cast<int>(y);
}

bool RasterPageTextToL8(const Page& page, const lv_font_t* font, lv_coord_t line_gap,
                        lv_coord_t para_gap, int page_w, int page_h, int underline_mode,
                        RasterImage& out, bool antialias) {
    out.Reset();
    if (font == nullptr || page_w <= 0 || page_h <= 0 || !PageIsTextOnly(page)) {
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    const size_t n = static_cast<size_t>(page_w) * static_cast<size_t>(page_h);
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
    out.height = static_cast<uint16_t>(page_h);

    const lv_coord_t line_h = font->line_height > 0 ? font->line_height : 24;
    const lv_coord_t indent_px = ParaIndentPadPx(font);
    lv_coord_t y = 0;
    bool any = false;

    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) {
            continue;
        }
        if (item.para_gap_before && any) {
            const lv_coord_t extra =
                (para_gap > line_gap) ? static_cast<lv_coord_t>(para_gap - line_gap) : 0;
            y = static_cast<lv_coord_t>(y + extra);
        }
        if (y + line_h > page_h) {
            break;
        }

        lv_coord_t text_x = 0;
        const char* txt = item.text.c_str();
        // 章名可能含「第N章\\n章节名」：按行画并垂直推进
        if (item.chapter_title && std::strchr(txt, '\n') != nullptr) {
            const char* p = txt;
            lv_coord_t line_y = y;
            int end_x = 0;
            while (p != nullptr && *p != '\0') {
                const char* nl = std::strchr(p, '\n');
                std::string line = nl ? std::string(p, static_cast<size_t>(nl - p)) : std::string(p);
                const lv_coord_t tw = MeasureTextW(font, line.c_str());
                text_x = tw < page_w ? static_cast<lv_coord_t>((page_w - tw) / 2) : 0;
                if (line_y + line_h > page_h) {
                    break;
                }
                if (!DrawUtf8Line(out, font, text_x, static_cast<int>(line_y), line.c_str(), true,
                                  &end_x, false, antialias)) {
                    out.Reset();
                    return false;
                }
                line_y = static_cast<lv_coord_t>(line_y + line_h + line_gap);
                p = nl ? nl + 1 : nullptr;
            }
            y = line_y;
            any = true;
            continue;
        }
        if (item.chapter_title) {
            const lv_coord_t tw = MeasureTextW(font, txt);
            text_x = tw < page_w ? static_cast<lv_coord_t>((page_w - tw) / 2) : 0;
        } else if (item.para_indent) {
            text_x = indent_px;
        }

        int end_x = text_x;
        if (!DrawUtf8Line(out, font, text_x, static_cast<int>(y), txt, item.chapter_title, &end_x, false, antialias)) {
            out.Reset();
            return false;
        }
        if (underline_mode > 0) {
            const int baseline_y = static_cast<int>(y) + static_cast<int>(line_h - font->base_line);
            const int ul_y = baseline_y - font->underline_position + 4;
            DrawUnderline(out, text_x, end_x - 1, ul_y, underline_mode);
        }
        y = static_cast<lv_coord_t>(y + line_h + line_gap);
        any = true;
    }

    FinishTextRaster(out, antialias);
    ESP_LOGI(TAG, "raster %dx%d items=%u cost=%lldms", page_w, page_h,
             static_cast<unsigned>(page.items.size()),
             static_cast<long long>((esp_timer_get_time() - t0) / 1000));
    return any;
}

bool RasterPageVerticalTextToL8(const Page& page, const lv_font_t* font, lv_coord_t line_gap,
                                lv_coord_t para_gap, int page_w, int page_h, int rule_mode,
                                RasterImage& out, bool antialias) {
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
    int x = 0;
    bool any = false, previous_title = false;
    for (const auto& item : page.items) {
        if (item.kind != ContentKind::kText || item.text.empty()) continue;
        if (any && item.para_gap_before) {
            x += para_gap;
            if (previous_title) x += kReaderChapterTitleAfterGap;
        }
        if (x + cell > page_w) { out.Reset(); return false; }
        const int top = item.para_indent && !item.chapter_title
            ? std::min(2 * cell, std::max(0, page_h - cell)) : 0;
        if (!DrawUtf8Line(out, font, x, top, item.text.c_str(), item.chapter_title, nullptr, true, antialias)) {
            out.Reset(); return false;
        }
        if (rule_mode > 0) {
            for (int y = 0; y < page_h; ++y) {
                if (rule_mode != 2 || y % 10 < 6) PlotInk(out, x + cell + line_gap / 2, y);
            }
        }
        x += cell + line_gap;
        previous_title = item.chapter_title;
        any = true;
    }
    FinishTextRaster(out, antialias);
    return any;
}

}  // namespace reader
