#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "ui/apps/book/cover/book_cover_style.h"
#include "ui/apps/book/cover/book_cover_geometry.h"
#include "ui/common/drawing/ui_theme.h"

// 实心墨点（无 AA），直线/圆弧同一套，避免墨色深浅不一
static void CoverInkFill(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2,
                         lv_coord_t y2) {
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

// Share immutable masks between equal-sized covers; retain no pixel data after
// the last cover using a size is deleted. LVGL applies the mask to the complete
// object layer (background, children and border), before compositing below UI.
struct BookCoverStyleCoverMask {
    lv_image_dsc_t image{};
    uint8_t* pixels = nullptr;

    ~BookCoverStyleCoverMask() {
        lv_image_cache_drop(&image);
        lv_image_header_cache_drop(&image);
        lv_free(pixels);
    }
};

static std::vector<std::weak_ptr<BookCoverStyleCoverMask>> s_masks;

struct BookCoverStyleCoverFrameState {
    std::shared_ptr<BookCoverStyleCoverMask> mask;
};

static std::shared_ptr<BookCoverStyleCoverMask> GetCoverMask(int width, int height) {
    for (auto it = s_masks.begin(); it != s_masks.end();) {
        if (auto mask = it->lock()) {
            if (mask->image.header.w == width && mask->image.header.h == height) {
                return mask;
            }
            ++it;
        } else {
            it = s_masks.erase(it);
        }
    }
    auto mask = std::make_shared<BookCoverStyleCoverMask>();
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_A8);
    const size_t bytes = size_t(stride) * height;
    mask->pixels = static_cast<uint8_t*>(lv_malloc(bytes));
    LV_ASSERT_MALLOC(mask->pixels);
    if (!mask->pixels) {
        return {};
    }
    std::memset(mask->pixels, 255, bytes);
    const int radius = BookCoverCornerRadius(width, height), cx = width - 1 - radius;
    // Match the existing border's integer circle exactly. Left corners stay square.
    for (int dy = 0; dy < radius; ++dy) {
        const int t = radius - dy;
        const int first = cx + int(std::floor(std::sqrt(float(radius * radius - t * t)))) + 1;
        std::memset(mask->pixels + size_t(dy) * stride + first, 0, width - first);
        std::memset(mask->pixels + size_t(height - 1 - dy) * stride + first, 0, width - first);
    }
    auto& image = mask->image;
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_A8;
    image.header.w = width;
    image.header.h = height;
    image.header.stride = stride;
    image.data = mask->pixels;
    image.data_size = bytes;
    s_masks.push_back(mask);
    return mask;
}

static void UpdateCoverMask(lv_obj_t* obj, BookCoverStyleCoverFrameState& state) {
    const int width = lv_obj_get_width(obj), height = lv_obj_get_height(obj);
    if (width <= 0 || height <= 0) {
        return;
    }
    if (state.mask && state.mask->image.header.w == width && state.mask->image.header.h == height) {
        return;
    }
    auto mask = GetCoverMask(width, height);
    if (!mask) {
        return;
    }
    lv_obj_set_style_bitmap_mask_src(obj, &mask->image, 0);
    state.mask = std::move(mask);
}

// 右上/右下四分之一圆外轮廓：Bresenham 连续实心点，与直线边重叠 1px 衔接
static void DrawBookCoverCornerArcs(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink, const lv_area_t& box,
                                    lv_coord_t r) {
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;
    const lv_coord_t cy_top = box.y1 + r;
    const lv_coord_t cy_bot = box.y2 - r;
    // Rasterize the annulus as solid scanlines inside the cover bounds.
    const int inner = std::max<int>(0, r - bw);
    for (int dy = 0; dy <= r; ++dy) {
        const int outer_x = static_cast<int>(std::floor(std::sqrt(static_cast<float>(r * r - dy * dy))));
        const int inner_x =
            dy < inner ? static_cast<int>(std::floor(std::sqrt(static_cast<float>(inner * inner - dy * dy)))) + 1 : 0;
        const int first = std::min<int>(outer_x, inner_x);
        CoverInkFill(layer, ink, cx + first, cy_top - dy, cx + outer_x, cy_top - dy);
        CoverInkFill(layer, ink, cx + first, cy_bot + dy, cx + outer_x, cy_bot + dy);
    }
}

void DrawBookCoverBorder(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = UiColorForeground();
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
    const lv_coord_t r = BookCoverCornerRadius(w, h);
    DrawBookCoverBorder(layer, box, r);
}

static void OnCoverFrameEvent(lv_event_t* e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* state = static_cast<BookCoverStyleCoverFrameState*>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
        case LV_EVENT_SIZE_CHANGED:
            UpdateCoverMask(obj, *state);
            break;
        case LV_EVENT_DRAW_POST_END:
            OnBookCoverFrameDraw(e);
            break;
        case LV_EVENT_DELETE:
            delete state;
            break;
        default:
            break;
    }
}

// Left spine remains square; native layer masking reveals any background at
// the right corners. Do not paint white to simulate transparent geometry.
void StyleBookCoverFrame(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        auto* event = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(event) == OnCoverFrameEvent) {
            UpdateCoverMask(obj, *static_cast<BookCoverStyleCoverFrameState*>(lv_event_dsc_get_user_data(event)));
            return;
        }
    }
    auto* state = new BookCoverStyleCoverFrameState;
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_clip_corner(obj, false, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, OnCoverFrameEvent, LV_EVENT_ALL, state);
    UpdateCoverMask(obj, *state);
}
