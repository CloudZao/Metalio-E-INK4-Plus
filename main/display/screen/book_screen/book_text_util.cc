#include "book_screen/book_text_util.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_home.h"

#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "reader/reader.h"
#include "reader/text_encoding.h"

void FormatHomeDurationShort(char* out, size_t out_sz, uint32_t seconds) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    if (seconds < 60u) {
        std::snprintf(out, out_sz, "0m");
        return;
    }
    const uint32_t total_min = seconds / 60u;
    if (total_min < 60u) {
        std::snprintf(out, out_sz, "%um", static_cast<unsigned>(total_min));
        return;
    }
    const double hours = static_cast<double>(seconds) / 3600.0;
    std::snprintf(out, out_sz, "%.1fh", hours);
}

// 首页继续阅读卡：100% 时不带小数（避免「已阅读100.0%」）
void FormatBookReadPctLabel(char* out, size_t out_sz, int pct_x10) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    if (pct_x10 < 0) {
        pct_x10 = 0;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    const char* fmt = Lang::Strings::BOOK_READ_PCT_FMT;
    const char* pct_start = std::strchr(fmt, '%');
    if (pct_x10 >= 1000 && pct_start != nullptr) {
        std::snprintf(out, out_sz, "%.*s100%%", static_cast<int>(pct_start - fmt), fmt);
        return;
    }
    std::snprintf(out, out_sz, fmt, pct_x10 / 10, pct_x10 % 10);
}

// 首页统计：不再全库 Peek .pos。 - 最近/继续：book_home_snapshot MRU（写进度时维护）；空则不展示
HomeStats CollectHomeStats(const std::vector<reader::BookInfo>& books) {
    HomeStats out;
    const auto agg = reader::book_home_snapshot::LoadAggregateStats();
    if (agg.valid) {
        out.total_seconds = agg.total_seconds;
        out.finished_count = agg.finished_count;
    }

    if (books.empty()) {
        return out;
    }

    std::vector<std::string> recent_abs;
    reader::book_home_snapshot::LoadRecentAbsPaths(recent_abs);
    if (recent_abs.empty()) {
        return out;
    }

    int recent_n = 0;
    auto push_recent = [&](int idx) {
        if (idx < 0 || recent_n >= 2) {
            return;
        }
        for (int k = 0; k < recent_n; ++k) {
            if (out.recent_indices[k] == idx) {
                return;
            }
        }
        out.recent_indices[recent_n++] = idx;
    };

    for (const std::string& path : recent_abs) {
        int found = -1;
        for (int i = 0; i < static_cast<int>(books.size()); ++i) {
            if (books[static_cast<size_t>(i)].path == path) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            continue;
        }
        push_recent(found);
        if (recent_n >= 2) {
            break;
        }
    }

    if (recent_n <= 0) {
        return out;
    }

    out.has_recent = true;
    out.continue_index = out.recent_indices[0];
    {
        reader::BookSession::ProgressPeek peek;
        const char* path = books[static_cast<size_t>(out.continue_index)].path.c_str();
        if (reader::book_progress_cache::TryGet(path, peek) && peek.progress_x10 >= 0) {
            out.continue_progress_x10 = peek.progress_x10;
        } else {
            out.continue_progress_x10 = reader::BookSession::PeekReadingProgressX10(path);
        }
    }
    return out;
}

lv_coord_t Book_ContentWidth() {
    return LV_HOR_RES - kListPad * 2;
}

lv_coord_t Book_MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

// 单行截断：过长则末尾加「…」，按像素宽度预算。
std::string TruncateTextToWidth(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (font == nullptr || max_w <= 0 || Book_MeasureTextWidth(font, text.c_str()) <= max_w) {
        return text;
    }
    constexpr const char* kEllipsis = "…";
    const lv_coord_t ellipsis_w = Book_MeasureTextWidth(font, kEllipsis);
    const lv_coord_t budget = max_w - ellipsis_w;
    if (budget <= 0) {
        return kEllipsis;
    }
    std::string out;
    out.reserve(text.size() + 3);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, text.size() - i, &cp);
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

// 文件名单行省略：保留后缀（.ext 或文件夹末尾 /），只截主体。
std::string TruncateFilenameKeepExt(const std::string& name, const lv_font_t* font,
                                   lv_coord_t max_w) {
    if (font == nullptr || max_w <= 0 || Book_MeasureTextWidth(font, name.c_str()) <= max_w) {
        return name;
    }
    constexpr const char* kEllipsis = "…";
    std::string suffix;
    std::string stem = name;
    if (!name.empty() && name.back() == '/') {
        suffix = "/";
        stem.pop_back();
    } else {
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            suffix = name.substr(dot);
            stem = name.substr(0, dot);
        }
    }

    const lv_coord_t suffix_w = Book_MeasureTextWidth(font, suffix.c_str());
    const lv_coord_t ellipsis_w = Book_MeasureTextWidth(font, kEllipsis);
    lv_coord_t stem_budget = max_w - suffix_w - ellipsis_w;
    if (stem_budget < 0) {
        stem_budget = 0;
    }

    std::string head;
    head.reserve(stem.size());
    const uint8_t* d = reinterpret_cast<const uint8_t*>(stem.data());
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < stem.size() && stem_budget > 0) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, stem.size() - i, &cp);
        if (n == 0) {
            break;
        }
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        if (gw <= 0) {
            gw = 1;
        }
        if (used + gw > stem_budget) {
            break;
        }
        head.append(stem, i, n);
        used += gw;
        i += n;
    }
    if (head.empty() && suffix_w + ellipsis_w > max_w) {
        return TruncateTextToWidth(name, font, max_w);
    }
    return head + kEllipsis + suffix;
}

void Book_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

lv_obj_t* Book_AddColRule(lv_obj_t* row) {
    lv_obj_t* host = lv_obj_create(row);
    lv_obj_remove_style_all(host);
    lv_obj_set_width(host, kColRuleW);
    lv_obj_set_height(host, kColRuleInset * 2 + 40);
    lv_obj_set_style_pad_ver(host, kColRuleInset, 0);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(host);

    lv_obj_t* bar = lv_obj_create(host);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kColRuleW, lv_pct(100));
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(bar);
    lv_obj_center(bar);
    return host;
}

void Book_FinishColRule(lv_obj_t* row, lv_obj_t* host) {
    if (row == nullptr || host == nullptr || !lv_obj_is_valid(row) || !lv_obj_is_valid(host)) {
        return;
    }
    lv_obj_update_layout(row);
    lv_coord_t max_h = 0;
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(row, i);
        if (ch == nullptr || ch == host) {
            continue;
        }
        const lv_coord_t h = lv_obj_get_height(ch);
        if (h > max_h) {
            max_h = h;
        }
    }
    const lv_coord_t min_h = kColRuleInset * 2 + 40;
    lv_obj_set_height(host, max_h > min_h ? max_h : min_h);
}

void Book_ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font) {
    auto& st = Book_State();
    if (parent == st.reader.content) {
        st.reader.open_bar = nullptr;
        st.reader.open_pct_lbl = nullptr;
    }
    lv_obj_clean(parent);
    // 提示页需屏幕居中；关掉 flex，避免 START 把文案顶到上方
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, font != nullptr ? font : Book_ListFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, Book_ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    Book_DisableScroll(label);
}

int Book_ListPageCount() {
    auto& st = Book_State();
    const size_t n = (st.shelf.shelf_view == kBookReaderShelfViewList) ? st.shelf.shelf_entries.size()
                                                                  : st.shelf.books.size();
    if (n == 0 || st.shelf.list_page_size <= 0) {
        return 1;
    }
    return static_cast<int>((n + static_cast<size_t>(st.shelf.list_page_size) - 1) /
                            static_cast<size_t>(st.shelf.list_page_size));
}

void Book_ClampListPage() {
    auto& st = Book_State();
    const int pages = Book_ListPageCount();
    if (st.shelf.list_page < 0) {
        st.shelf.list_page = 0;
    }
    if (st.shelf.list_page >= pages) {
        st.shelf.list_page = pages - 1;
    }
}

static int CachedProgressX10(const char* book_abs_path) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return -1;
    }
    reader::BookSession::ProgressPeek peek;
    if (!reader::book_progress_cache::TryGet(book_abs_path, peek) || peek.progress_x10 < 0) {
        return -1;
    }
    return peek.progress_x10;
}

int Book_CachedProgressX10OrZero(const char* book_abs_path) {
    const int v = CachedProgressX10(book_abs_path);
    if (v < 0) {
        return 0;
    }
    return v > 1000 ? 1000 : v;
}

// 18 号字常无 '%'：数字用 18@2，百分号手绘
static void OnPercentMarkDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const lv_coord_t w = lv_area_get_width(&box);
    const lv_coord_t h = lv_area_get_height(&box);
    if (w < 4 || h < 6) {
        return;
    }

    const lv_coord_t d = h / 5;
    const lv_coord_t dot = d < 2 ? 2 : d;

    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;
    ink.border_width = 0;
    ink.radius = LV_RADIUS_CIRCLE;

    lv_area_t top;
    top.x1 = box.x1;
    top.y1 = box.y1 + 1;
    top.x2 = box.x1 + dot - 1;
    top.y2 = box.y1 + dot;
    lv_draw_rect(layer, &ink, &top);

    lv_area_t bot;
    bot.x1 = box.x2 - dot + 1;
    bot.y1 = box.y2 - dot;
    bot.x2 = box.x2;
    bot.y2 = box.y2 - 1;
    lv_draw_rect(layer, &ink, &bot);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = 1;
    line.opa = LV_OPA_COVER;
    line.p1.x = box.x2;
    line.p1.y = box.y1 + 1;
    line.p2.x = box.x1;
    line.p2.y = box.y2 - 1;
    lv_draw_line(layer, &line);
}

static lv_obj_t* CreatePercentMark(lv_obj_t* parent, lv_coord_t h) {
    const lv_coord_t mark_h = h > 4 ? static_cast<lv_coord_t>(h - 2) : h;
    const lv_coord_t mark_w = mark_h * 3 / 5;
    lv_obj_t* mark = lv_obj_create(parent);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, mark_w < 4 ? 4 : mark_w, mark_h < 6 ? 6 : mark_h);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    Book_DisableScroll(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mark, OnPercentMarkDraw, LV_EVENT_DRAW_MAIN, nullptr);
    return mark;
}

static void FormatProgressNumber(char* out, size_t out_sz, int pct_x10) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    if (pct_x10 < 0) {
        pct_x10 = 0;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    if (pct_x10 >= 1000) {
        std::snprintf(out, out_sz, "100");
    } else {
        std::snprintf(out, out_sz, "%d.%d", pct_x10 / 10, pct_x10 % 10);
    }
}

lv_obj_t* Book_CreateProgressPctBadge(lv_obj_t* parent, int pct_x10) {
    if (parent == nullptr || pct_x10 < 0) {
        return nullptr;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    const lv_font_t* badge_font = fontpack_lv_font_get(18, 2);
    if (badge_font == nullptr) {
        badge_font = Book_ItemFont();
    }

    char txt[12];
    FormatProgressNumber(txt, sizeof(txt), pct_x10);

    // 固定长方形：高 28，宽再加约一半（避开多选勾选 24×24）
    const lv_coord_t line_h = badge_font != nullptr ? badge_font->line_height : 18;

    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, kProgressBadgeW, kProgressBadgeH);
    lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, lv_color_black(), 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_radius(badge, 0, 0);
    Book_DisableScroll(badge);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* row = lv_obj_create(badge);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 1, 0);
    Book_DisableScroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, badge_font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    CreatePercentMark(row, line_h);
    lv_obj_center(row);
    return badge;
}

// 书库/详情/正文共用：仅 BOOT 长按进百问；盖板 prev/next 长按连翻（vk_home 长按不进百问）

