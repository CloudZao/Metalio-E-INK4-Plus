// assistant_paginate.cc — split from assistant_screen.cc
#include "assistant_screen_priv.h"

#include "assistant_paginate.h"
#include "a2ui_math.h"
#include "text_encoding.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <esp_log.h>

// 按视口宽把 UTF-8 文本折成物理行（含 letter_space）。
void WrapTextToLines(std::string_view text, const lv_font_t* font, lv_coord_t letter_space,
                     lv_coord_t viewport_w, std::vector<std::string>& lines) {
    lines.clear();
    std::string line;
    lv_coord_t line_w = 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            lines.push_back(std::move(line));
            line.clear();
            line_w = 0;
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(data + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t cw = Assistant_GlyphWidth(font, cp) + (line.empty() ? 0 : letter_space);
        if (!line.empty() && line_w + cw > viewport_w) {
            lines.push_back(std::move(line));
            line.clear();
            line_w = 0;
        }
        if (line.empty()) {
            line_w = Assistant_GlyphWidth(font, cp);
        } else {
            line_w += cw;
        }
        line.append(text.data() + i, n);
        i += n;
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(std::move(line));
    }
}

std::string JoinLines(const std::vector<std::string>& lines, size_t begin, size_t end) {
    std::string out;
    for (size_t i = begin; i < end && i < lines.size(); ++i) {
        if (i > begin) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

// 按字节范围切片 RichText spans（UTF-8 字节偏移）。
SpirVec<SpanRun> SliceSpans(const SpirVec<SpanRun>& spans, size_t byte_begin,
                            size_t byte_end) {
    SpirVec<SpanRun> out;
    size_t off = 0;
    for (const auto& sp : spans) {
        const size_t sp_begin = off;
        const size_t sp_end = off + sp.text.size();
        off = sp_end;
        if (sp_end <= byte_begin || sp_begin >= byte_end) {
            continue;
        }
        const size_t a = byte_begin > sp_begin ? byte_begin - sp_begin : 0;
        const size_t b = byte_end < sp_end ? byte_end - sp_begin : sp.text.size();
        if (a >= b) {
            continue;
        }
        SpanRun cut;
        cut.bold = sp.bold;
        cut.text = sp.text.substr(a, b - a);
        out.push_back(std::move(cut));
    }
    return out;
}

std::string SpansConcat(const SpirVec<SpanRun>& spans) {
    std::string s;
    for (const auto& sp : spans) {
        s.append(sp.text.data(), sp.text.size());
    }
    return s;
}

lv_coord_t EstimateItemHeight(const FlowItem& item, lv_coord_t viewport_w, lv_coord_t viewport_h) {
    const lv_coord_t line_h = Assistant_FontLineHeight(Assistant_UiFont());
    switch (item.kind) {
        case FlowKind::MsgGap:
            return kBlockGap;
        case FlowKind::Divider:
            return kDividerH;
        case FlowKind::Spacer:
            return item.spacer_h > 0 ? item.spacer_h : kDefaultSpacerH;
        case FlowKind::CardBegin:
            return kCardPad; // pad_top 近似；真正高度在子项累计
        case FlowKind::CardEnd:
            return kCardPad;
        case FlowKind::Image: {
            lv_coord_t h = item.img_h > 0 ? item.img_h : kDefaultImgH;
            if (h > viewport_h) {
                h = viewport_h;
            }
            if (h < kDefaultImgMinH) {
                h = kDefaultImgMinH;
            }
            return h;
        }
        case FlowKind::Button:
            return kButtonH;
        case FlowKind::Badge:
            return line_h + kBadgeExtra;
        case FlowKind::Progress:
            return line_h + kProgressExtra + 18;
        case FlowKind::Status:
            return line_h;
        case FlowKind::Header: {
            lv_coord_t h = line_h + kHeaderExtra;
            if (!item.text2.empty()) {
                h += line_h + kHeaderSubGap;
            }
            return h;
        }
        case FlowKind::ListItem: {
            lv_coord_t h = line_h + kListItemExtra;
            if (!item.text2.empty()) {
                h += line_h + kHeaderSubGap;
            }
            return h;
        }
        case FlowKind::Text: {
            std::vector<std::string> lines;
            WrapTextToLines(item.text, Assistant_FontFor(item.bold), Assistant_VariantLetterSpace(item.variant.c_str()),
                            viewport_w, lines);
            return static_cast<lv_coord_t>(lines.size()) * line_h;
        }
        case FlowKind::RichText: {
            std::vector<std::string> lines;
            // 折行宽度按 regular 估算（bold 略宽，偏保守多留一行）
            WrapTextToLines(SpansConcat(item.spans), Assistant_UiFont(),
                            Assistant_VariantLetterSpace(item.variant.c_str()), viewport_w, lines);
            return static_cast<lv_coord_t>(lines.size()) * line_h;
        }
        case FlowKind::Math: {
            int32_t h = a2ui_math_measure_height(item.text.c_str(), item.math_display, viewport_w);
            if (h < line_h) {
                h = line_h * 2;
            }
            if (h > viewport_h) {
                h = viewport_h;
            }
            return static_cast<lv_coord_t>(h);
        }
    }
    return line_h;
}

bool IsTextLike(FlowKind k) {
    return k == FlowKind::Text || k == FlowKind::RichText;
}

void ReopenCards(FlowList& cur, int card_depth, lv_coord_t& y) {
    for (int i = 0; i < card_depth; ++i) {
        FlowItem begin;
        begin.kind = FlowKind::CardBegin;
        cur.push_back(std::move(begin));
        y += kCardPad;
    }
}

void NotePageContent(size_t fi) {
    if (Assistant_State().layout.open_page_flow_begin == static_cast<size_t>(-1)) {
        Assistant_State().layout.open_page_flow_begin = fi;
    }
}

void CommitPage(FlowList& cur) {
    if (cur.empty()) {
        return;
    }
    const size_t begin =
        Assistant_State().layout.open_page_flow_begin == static_cast<size_t>(-1) ? Assistant_State().layout.building_flow_fi : Assistant_State().layout.open_page_flow_begin;
    Assistant_State().layout.pages.push_back(std::move(cur));
    Assistant_State().layout.page_flow_begin.push_back(begin);
    Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);
    cur.clear();
}

void PushPage(FlowList& cur, int card_depth, lv_coord_t& y) {
    if (cur.empty()) {
        return;
    }
    CommitPage(cur);
    y = 0;
    ReopenCards(cur, card_depth, y);
}

int CardDepthBefore(size_t fi) {
    int d = 0;
    for (size_t i = 0; i < fi && i < Assistant_State().layout.flow.size(); ++i) {
        if (Assistant_State().layout.flow[i].kind == FlowKind::CardBegin) {
            ++d;
        } else if (Assistant_State().layout.flow[i].kind == FlowKind::CardEnd && d > 0) {
            --d;
        }
    }
    return d;
}

// 把可折行文本块按行装进页面，保证不半行截断。first_page_out：本项首次入页的页码。
void PaginateTextLike(const FlowItem& item, FlowList& cur, lv_coord_t& y,
                      lv_coord_t viewport_w, lv_coord_t viewport_h, int card_depth,
                      int* first_page_out) {
    const lv_coord_t line_h = Assistant_FontLineHeight(Assistant_UiFont());
    std::vector<std::string> lines;
    std::string full;
    if (item.kind == FlowKind::RichText) {
        full = SpansConcat(item.spans);
        WrapTextToLines(full, Assistant_UiFont(), Assistant_VariantLetterSpace(item.variant.c_str()), viewport_w, lines);
    } else {
        full.assign(item.text.data(), item.text.size());
        WrapTextToLines(item.text, Assistant_FontFor(item.bold), Assistant_VariantLetterSpace(item.variant.c_str()),
                        viewport_w, lines);
    }

    // 与 Wrap 同步扫描：建立每物理行在 full 中的字节区间
    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(lines.size());
    size_t scan = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& ln = lines[li];
        if (ln.empty()) {
            ranges.push_back({scan, scan});
            if (scan < full.size() && full[scan] == '\n') {
                ++scan;
            }
            continue;
        }
        if (scan + ln.size() <= full.size() && full.compare(scan, ln.size(), ln) == 0) {
            ranges.push_back({scan, scan + ln.size()});
            scan += ln.size();
        } else {
            const size_t begin = full.find(ln, scan);
            if (begin == std::string::npos) {
                ranges.push_back({scan, scan});
            } else {
                ranges.push_back({begin, begin + ln.size()});
                scan = begin + ln.size();
            }
        }
        if (scan < full.size() && full[scan] == '\n') {
            ++scan;
        }
    }

    size_t i = 0;
    while (i < lines.size()) {
        if (y + line_h > viewport_h && !cur.empty()) {
            PushPage(cur, card_depth, y);
        }
        size_t take = 0;
        lv_coord_t used = 0;
        while (i + take < lines.size()) {
            if (y + used + line_h > viewport_h) {
                break;
            }
            used += line_h;
            ++take;
        }
        if (take == 0) {
            take = 1;
            used = line_h;
        }

        FlowItem slice = item;
        if (item.kind == FlowKind::RichText) {
            const size_t b0 = ranges[i].first;
            const size_t b1 = ranges[i + take - 1].second;
            slice.spans = SliceSpans(item.spans, b0, b1);
            slice.text.clear();
        } else {
            const std::string joined = JoinLines(lines, i, i + take);
            slice.text.assign(joined.data(), joined.size());
        }
        cur.push_back(std::move(slice));
        NotePageContent(Assistant_State().layout.building_flow_fi);
        if (first_page_out != nullptr && *first_page_out < 0) {
            *first_page_out = static_cast<int>(Assistant_State().layout.pages.size());
        }
        y += used;
        i += take;
        if (i < lines.size()) {
            PushPage(cur, card_depth, y);
        }
    }
}

// 拆出循环体：避免 xtensa-gcc 在 Assistant_RebuildPages 的 lambda+CFG 上 ICE（fwprop1/try_forward_edges）
void EnsureRoom(FlowList& cur, lv_coord_t& y, int card_depth, lv_coord_t vh, lv_coord_t need) {
    if (y + need > vh && !cur.empty()) {
        PushPage(cur, card_depth, y);
    }
}

void MarkFlowFirstPage(size_t fi) {
    NotePageContent(fi);
    if (fi < Assistant_State().layout.flow_first_page.size() && Assistant_State().layout.flow_first_page[fi] < 0) {
        Assistant_State().layout.flow_first_page[fi] = static_cast<int>(Assistant_State().layout.pages.size());
    }
}

void AppendOneFlowItem(size_t fi, FlowList& cur, lv_coord_t& y, int& card_depth, lv_coord_t vw,
                       lv_coord_t vh) {
    Assistant_State().layout.building_flow_fi = fi;
    const auto& item = Assistant_State().layout.flow[fi];
    if (item.kind == FlowKind::MsgGap) {
        if (!cur.empty()) {
            y += kBlockGap;
        }
        return;
    }
    if (item.kind == FlowKind::CardBegin) {
        EnsureRoom(cur, y, card_depth, vh, kCardPad + Assistant_FontLineHeight(Assistant_UiFont()));
        MarkFlowFirstPage(fi);
        cur.push_back(item);
        ++card_depth;
        y += kCardPad;
        return;
    }
    if (item.kind == FlowKind::CardEnd) {
        MarkFlowFirstPage(fi);
        cur.push_back(item);
        if (card_depth > 0) {
            --card_depth;
        }
        y += kCardPad;
        return;
    }
    if (IsTextLike(item.kind)) {
        PaginateTextLike(item, cur, y, vw, vh, card_depth, &Assistant_State().layout.flow_first_page[fi]);
        if (!cur.empty()) {
            y += kBlockGap;
        }
        return;
    }

    const lv_coord_t h = EstimateItemHeight(item, vw, vh);
    EnsureRoom(cur, y, card_depth, vh, h);
    if (h > vh && !cur.empty()) {
        PushPage(cur, card_depth, y);
    }
    MarkFlowFirstPage(fi);
    cur.push_back(item);
    y += h + kBlockGap;
}

void Assistant_ClampPageIndex() {
    if (Assistant_State().layout.page_index >= static_cast<int>(Assistant_State().layout.pages.size())) {
        Assistant_State().layout.page_index = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
    }
    if (Assistant_State().layout.page_index < 0) {
        Assistant_State().layout.page_index = 0;
    }
}

void Assistant_RebuildPages() {
    Assistant_State().layout.pages.clear();
    Assistant_State().layout.page_flow_begin.clear();
    Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);
    Assistant_State().layout.flow_first_page.assign(Assistant_State().layout.flow.size(), -1);
    const lv_coord_t vw =
        Assistant_State().layout.viewport_w > 0 ? Assistant_State().layout.viewport_w : (LV_HOR_RES - kContentPadX * 2);
    const lv_coord_t vh = Assistant_State().layout.viewport_h > 0 ? Assistant_State().layout.viewport_h
                                                                  : (LV_VER_RES - kFooterH - kContentPadY * 2);

    FlowList cur;
    lv_coord_t y = 0;
    int card_depth = 0;
    for (size_t fi = 0; fi < Assistant_State().layout.flow.size(); ++fi) {
        AppendOneFlowItem(fi, cur, y, card_depth, vw, vh);
    }
    CommitPage(cur);
    if (Assistant_State().layout.pages.empty()) {
        Assistant_State().layout.pages.push_back({});
        Assistant_State().layout.page_flow_begin.push_back(0);
    }
    Assistant_ClampPageIndex();
    ESP_LOGI(TAG, "paginated %u pages (flow=%u, vw=%d vh=%d)",
             static_cast<unsigned>(Assistant_State().layout.pages.size()), static_cast<unsigned>(Assistant_State().layout.flow.size()),
             static_cast<int>(vw), static_cast<int>(vh));
}

int Assistant_RebuildPagesTail() {
    if (Assistant_State().layout.pages.empty() || Assistant_State().layout.page_flow_begin.size() != Assistant_State().layout.pages.size()) {
        Assistant_RebuildPages();
        return 0;
    }
    size_t fi0 = Assistant_State().layout.page_flow_begin.back();
    if (fi0 >= Assistant_State().layout.flow.size()) {
        Assistant_RebuildPages();
        return 0;
    }
    int page0 = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
    // 末页若是长文续页，须退回到该 flow 项首次入页处再排
    if (fi0 < Assistant_State().layout.flow_first_page.size() && Assistant_State().layout.flow_first_page[fi0] >= 0 &&
        Assistant_State().layout.flow_first_page[fi0] < page0) {
        page0 = Assistant_State().layout.flow_first_page[fi0];
        fi0 = Assistant_State().layout.page_flow_begin[static_cast<size_t>(page0)];
    }
    if (page0 < 0 || static_cast<size_t>(page0) > Assistant_State().layout.pages.size() || fi0 >= Assistant_State().layout.flow.size()) {
        Assistant_RebuildPages();
        return 0;
    }

    Assistant_State().layout.pages.resize(static_cast<size_t>(page0));
    Assistant_State().layout.page_flow_begin.resize(static_cast<size_t>(page0));
    Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);

    if (Assistant_State().layout.flow_first_page.size() < Assistant_State().layout.flow.size()) {
        Assistant_State().layout.flow_first_page.resize(Assistant_State().layout.flow.size(), -1);
    }
    for (size_t i = 0; i < Assistant_State().layout.flow_first_page.size(); ++i) {
        if (i >= fi0 || Assistant_State().layout.flow_first_page[i] >= page0) {
            Assistant_State().layout.flow_first_page[i] = -1;
        }
    }

    const lv_coord_t vw =
        Assistant_State().layout.viewport_w > 0 ? Assistant_State().layout.viewport_w : (LV_HOR_RES - kContentPadX * 2);
    const lv_coord_t vh = Assistant_State().layout.viewport_h > 0 ? Assistant_State().layout.viewport_h
                                                                  : (LV_VER_RES - kFooterH - kContentPadY * 2);

    FlowList cur;
    lv_coord_t y = 0;
    int card_depth = CardDepthBefore(fi0);
    if (card_depth > 0) {
        ReopenCards(cur, card_depth, y);
    }
    for (size_t fi = fi0; fi < Assistant_State().layout.flow.size(); ++fi) {
        AppendOneFlowItem(fi, cur, y, card_depth, vw, vh);
    }
    CommitPage(cur);
    if (Assistant_State().layout.pages.empty()) {
        Assistant_State().layout.pages.push_back({});
        Assistant_State().layout.page_flow_begin.push_back(0);
        page0 = 0;
    }
    Assistant_ClampPageIndex();
    ESP_LOGI(TAG, "paginated tail flow=%u page=%d -> %u pages (flow=%u)",
             static_cast<unsigned>(fi0), page0, static_cast<unsigned>(Assistant_State().layout.pages.size()),
             static_cast<unsigned>(Assistant_State().layout.flow.size()));
    return page0;
}

