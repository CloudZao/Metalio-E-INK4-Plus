#include "home_screen/home_screen_priv.h"

#include "home_screen_priv.h"
#include "home_card_style.h"
#include <cstring>

#include <lvgl.h>

const lv_image_dsc_t *Home_GrayDitherImg()
    {
        if (!Home_gray_dither_ready)
        {
            for (int y = 0; y < kDitherH; ++y)
            {
                for (int x = 0; x < kDitherW; ++x)
                {
                    const bool black = kBayer4[y & 3][x & 3] < kBayerThreshold;
                    Home_gray_dither_l8[y * kDitherW + x] = black ? 0x00 : 0xFF;
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

    void Home_ApplyCardStyle(lv_obj_t *cell, int style)
    {
        if (style == HomeScreen::kCardStyleGray)
        {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_image_src(cell, Home_GrayDitherImg(), 0);
            lv_obj_set_style_bg_image_tiled(cell, true, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            return;
        }

        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_image_src(cell, nullptr, 0);
        if (style == HomeScreen::kCardStyleBorder)
        {
            lv_obj_set_style_border_width(cell, HOME_APP_CARD_BORDER_W, 0);
            lv_obj_set_style_border_color(cell, lv_color_black(), 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
        }
        else
        {
            lv_obj_set_style_border_width(cell, 0, 0);
        }
    }

