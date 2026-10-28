#include "ui_qrcode.h"

namespace {
constexpr lv_coord_t kBorder = 2; // 二维码外框线宽
constexpr lv_coord_t kPadding = 3; // 内容与外框的间隙
constexpr lv_coord_t kRadius = 16; // 外框圆角
}

lv_obj_t* UiCreateQrFrame(lv_obj_t* parent, lv_coord_t side) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, side + 2 * (kPadding + kBorder), side + 2 * (kPadding + kBorder));
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, kRadius, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_border_width(box, kBorder, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(box, kPadding, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return box;
}
