#include <cstring>
#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "ui/home/home_screen_priv.h"
#include "ui/common/drawing/ui_theme.h"

const lv_image_dsc_t* Home_GrayDitherImg() {
    static bool was_dark = false;
    const bool dark = UiThemeIsDark();
    if (!Home_gray_dither_ready || was_dark != dark) {
        lv_image_cache_drop(&Home_gray_dither_img);
        was_dark = dark;
        for (int y = 0; y < kDitherH; ++y) {
            for (int x = 0; x < kDitherW; ++x) {
                const bool black = kBayer4[y & 3][x & 3] < kBayerThreshold;
                Home_gray_dither_l8[y * kDitherW + x] = (black != dark) ? 0x00 : 0xFF;
            }
        }
        Home_gray_dither_img.header.magic = LV_IMAGE_HEADER_MAGIC;
        Home_gray_dither_img.header.cf = LV_COLOR_FORMAT_L8;
        Home_gray_dither_img.header.flags = 0;
        Home_gray_dither_img.header.w = kDitherW;
        Home_gray_dither_img.header.h = kDitherH;
        Home_gray_dither_img.header.stride = kDitherW;
        Home_gray_dither_img.data_size = sizeof(Home_gray_dither_l8);
        Home_gray_dither_img.data = Home_gray_dither_l8;
        Home_gray_dither_ready = true;
    }
    return &Home_gray_dither_img;
}

void Home_ApplyCardStyle(lv_obj_t* cell, int style) {
    if (style == HomeScreen::kCardStyleGray) {
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_image_src(cell, Home_GrayDitherImg(), 0);
        lv_obj_set_style_bg_image_tiled(cell, true, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        return;
    }

    lv_obj_set_style_bg_color(cell, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(cell, nullptr, 0);
    if (style == HomeScreen::kCardStyleBorder) {
        lv_obj_set_style_border_width(cell, HOME_APP_CARD_BORDER_W, 0);
        lv_obj_set_style_border_color(cell, UiColorForeground(), 0);
        lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
    }
}
