#include "book_screen/cover/book_cover_style.h"
#include "book_screen/book_screen_priv.h"

#include <lvgl.h>
#include <algorithm>
#include <cmath>

static lv_coord_t BookCoverRightRadius(lv_coord_t w, lv_coord_t h) {
    lv_coord_t r = w / 6;
    if (r < 10) {
        r = 10;
    }
    const lv_coord_t max_r = std::min(w, h) / 2 - 2;
    if (max_r > 0 && r > max_r) {
        r = max_r;
    }
    return r > 0 ? r : 1;
}

// 实心墨点（无 AA），直线/圆弧同一套，避免墨色深浅不一
static void CoverInkFill(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink, lv_coord_t x1,
                         lv_coord_t y1, lv_coord_t x2, lv_coord_t y2) {
    if (x1 > x2 || y1 > y2) {
        return;
    }
    lv_area_t a;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &ink, &a);
}

// 擦掉右上/右下「方角耳朵」：圆外整行涂白（与描边共用圆心半径）
void EraseBookCoverRightEars(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_white();
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;
    rd.radius = 0;
    const lv_coord_t cx = box.x2 - r;
    const int rr = static_cast<int>(r) * static_cast<int>(r);
    for (lv_coord_t dy = 0; dy < r; ++dy) {
        const int t = static_cast<int>(r - dy);
        // 圆内最右像素 = cx+floor(sqrt)；其右一列起为耳朵
        const lv_coord_t x0 =
            cx + static_cast<lv_coord_t>(std::floor(std::sqrt(static_cast<float>(rr - t * t)))) + 1;
        if (x0 > box.x2) {
            continue;
        }
        CoverInkFill(layer, rd, x0, box.y1 + dy, box.x2, box.y1 + dy);
        CoverInkFill(layer, rd, x0, box.y2 - dy, box.x2, box.y2 - dy);
    }
}

// 右上/右下四分之一圆外轮廓：Bresenham 连续实心点，与直线边重叠 1px 衔接
static void DrawBookCoverCornerArcs(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink,
                                    const lv_area_t& box, lv_coord_t r) {
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;
    const lv_coord_t cy_top = box.y1 + r;
    const lv_coord_t cy_bot = box.y2 - r;
    auto plot = [&](lv_coord_t x, lv_coord_t y) {
        if (x < box.x1 || x > box.x2 || y < box.y1 || y > box.y2) {
            return;
        }
        CoverInkFill(layer, ink, x, y, x + bw - 1, y + bw - 1);
    };

    int x = 0;
    int y = static_cast<int>(r);
    int err = 1 - y;
    while (x <= y) {
        plot(cx + x, cy_top - y);
        plot(cx + y, cy_top - x);
        plot(cx + x, cy_bot + y);
        plot(cx + y, cy_bot + x);
        if (err < 0) {
            err += 2 * x + 3;
        } else {
            err += 2 * (x - y) + 5;
            --y;
        }
        ++x;
    }
}

void DrawBookCoverBorder(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;
    ink.border_width = 0;
    ink.radius = 0;
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;

    // 左边框（整高）
    CoverInkFill(layer, ink, box.x1, box.y1, box.x1 + bw - 1, box.y2);
    // 顶/底直线接到圆角起点（含 cx，与弧端点重叠）
    CoverInkFill(layer, ink, box.x1, box.y1, cx, box.y1 + bw - 1);
    CoverInkFill(layer, ink, box.x1, box.y2 - bw + 1, cx, box.y2);
    // 右侧直线（含圆角端点行，与弧重叠）
    CoverInkFill(layer, ink, box.x2 - bw + 1, box.y1 + r, box.x2, box.y2 - r);

    // 书脊细线（左边框内侧）
    CoverInkFill(layer, ink, box.x1 + kCoverSpineInset, box.y1 + bw,
                 box.x1 + kCoverSpineInset + bw - 1, box.y2 - bw);

    DrawBookCoverCornerArcs(layer, ink, box, r);
}

void OnBookCoverFrameDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const lv_coord_t w = lv_area_get_width(&box);
    const lv_coord_t h = lv_area_get_height(&box);
    if (w < 8 || h < 8) {
        return;
    }
    const lv_coord_t r = BookCoverRightRadius(w, h);
    EraseBookCoverRightEars(layer, box, r);
    DrawBookCoverBorder(layer, box, r);
}

// 封面外框：左直脊、右圆角（子控件绘完后再切角描边）
void StyleBookCoverFrame(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_clip_corner(obj, false, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, OnBookCoverFrameDraw, LV_EVENT_DRAW_POST_END, nullptr);
}

