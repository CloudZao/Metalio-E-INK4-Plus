#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include "ui/common/widgets/screen_layout.h"
#include "ui/common/widgets/ui_qrcode.h"

static constexpr lv_coord_t kBorder = 2;                           // 二维码外框线宽
static constexpr lv_coord_t kPadding = kUiQrFrameInset - kBorder;  // 内容与外框的间隙
static constexpr lv_coord_t kRadius = 16;                          // 外框圆角

static lv_obj_t* CreateQrFrame(lv_obj_t* parent, lv_coord_t side) {
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

lv_obj_t* UiCreateQrCode(lv_obj_t* parent, lv_coord_t side) {
    auto* frame = CreateQrFrame(parent, side);
    auto* qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(qr, side);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_clear_flag(qr, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return qr;
}

void UiResizeQrCode(lv_obj_t* qr, lv_coord_t side) {
    if (side < 1) {
        side = 1;
    }
    lv_obj_set_size(lv_obj_get_parent(qr), side + 2 * kUiQrFrameInset, side + 2 * kUiQrFrameInset);
    if (lv_obj_get_width(qr) != side) {
        lv_qrcode_set_size(qr, side);
    }
}

void ApQrPaint(lv_obj_t* qr, const char* text) {
    if (qr == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    if (lv_qrcode_update(qr, text, std::strlen(text)) != LV_RESULT_OK) {
        ESP_LOGW("UiQr", "encoding failed: side=%ld", static_cast<long>(lv_obj_get_width(qr)));
    }
}

lv_obj_t* ApQrCreate(lv_obj_t* parent) {
    int width = ScreenContentSize(parent).width;
    if (width <= 0) {
        width = lv_display_get_horizontal_resolution(lv_display_get_default());
    }
    const int height = lv_display_get_vertical_resolution(lv_display_get_default());
    const int side = std::max(1, std::min<int>({240, height / 5, width - 2 * kUiQrFrameInset}));
    return UiCreateQrCode(parent, side);
}
