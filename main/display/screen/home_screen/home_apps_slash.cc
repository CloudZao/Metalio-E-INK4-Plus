#include "home_screen/home_screen_priv.h"

#include "home_screen_priv.h"
#include "home_screen_ui.h"
#include "home_apps_slash.h"
#include "home_screen/home_hero.h"
#include "assistant_screen/assistant_screen.h"
#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "cloud_screen/cloud_screen.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "task_screen/task_screen.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"
#include "wallpaper_screen/wallpaper_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

    void Home_FillSlashAppsPage()
    {
        if (Home_apps == nullptr || Home_slash_apps_h <= 0)
        {
            ESP_LOGW(TAG, "slash apps skip h=%d", static_cast<int>(Home_slash_apps_h));
            return;
        }
        lv_obj_clean(Home_apps);
        const lv_coord_t inner_h =
            Home_slash_apps_h - HOME_SLASH_ROW_GAP * (HOME_SLASH_ROWS - 1);
        const lv_coord_t row_h = inner_h / HOME_SLASH_ROWS;
        // 左右斜边平行，水平间隙 = COL_GAP（与行间距同）；bbox 仍重叠 SLANT-COL_GAP
        const lv_coord_t cell_w =
            (LV_HOR_RES - 2 * HOME_SLASH_SIDE_PAD + HOME_SLASH_SLANT - HOME_SLASH_COL_GAP) / 2;
        for (int r = 0; r < HOME_SLASH_ROWS; ++r)
        {
            lv_obj_t *row = lv_obj_create(Home_apps);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_HOR_RES, row_h);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

            const int base = r * 2;
            if (base < kSlashAppCount)
            {
                Home_CreateSlashAppCell(row, kSlashApps[base], r, 0, row_h, cell_w);
            }
            if (base + 1 < kSlashAppCount)
            {
                Home_CreateSlashAppCell(row, kSlashApps[base + 1], r, 1, row_h, cell_w);
            }
        }
        if (Home_page_lbl != nullptr)
        {
            lv_obj_add_flag(Home_page_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "slash apps %d row_h=%d cell_w=%d", kSlashAppCount, static_cast<int>(row_h),
                 static_cast<int>(cell_w));
    }

    void Home_OnSlashCellDraw(lv_event_t *e)
    {
        if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN)
        {
            return;
        }
        lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
        lv_layer_t *layer = lv_event_get_layer(e);
        if (obj == nullptr || layer == nullptr)
        {
            return;
        }
        const intptr_t flags = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
        const bool left = (flags & 1) != 0;
        const bool black = (flags & 2) != 0;
        // 用整格坐标画梯形；content_coords 会扣掉 pad，中缝会被垫宽
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        const lv_coord_t slant = HOME_SLASH_SLANT;

        lv_point_precise_t p[4];
        if (left)
        {
            // 左卡：右缘为反斜线
            p[0].x = a.x1;
            p[0].y = a.y1;
            p[1].x = a.x2;
            p[1].y = a.y1;
            p[2].x = a.x2 - slant;
            p[2].y = a.y2;
            p[3].x = a.x1;
            p[3].y = a.y2;
        }
        else
        {
            // 右卡：左缘为反斜线
            p[0].x = a.x1 + slant;
            p[0].y = a.y1;
            p[1].x = a.x2;
            p[1].y = a.y1;
            p[2].x = a.x2;
            p[2].y = a.y2;
            p[3].x = a.x1;
            p[3].y = a.y2;
        }

        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = black ? lv_color_black() : lv_color_white();
        tri.opa = LV_OPA_COVER;
        tri.p[0] = p[0];
        tri.p[1] = p[1];
        tri.p[2] = p[2];
        lv_draw_triangle(layer, &tri);
        tri.p[0] = p[0];
        tri.p[1] = p[2];
        tri.p[2] = p[3];
        lv_draw_triangle(layer, &tri);

        if (!black)
        {
            // 白卡四边同宽描边（含斜边）
            lv_draw_line_dsc_t line;
            lv_draw_line_dsc_init(&line);
            line.color = lv_color_black();
            line.opa = LV_OPA_COVER;
            line.width = HOME_SLASH_BORDER_W;
            line.round_start = 0;
            line.round_end = 0;
            for (int i = 0; i < 4; ++i)
            {
                line.p1 = p[i];
                line.p2 = p[(i + 1) % 4];
                lv_draw_line(layer, &line);
            }
        }
    }

    void Home_CreateSlashAppCell(lv_obj_t *row, const AppEntry &entry, int row_i, int col,
                            lv_coord_t row_h, lv_coord_t cell_w)
    {
        const bool left = (col == 0);
        const bool black = ((row_i + col) % 2) == 0;
        const intptr_t flags = (left ? 1 : 0) | (black ? 2 : 0);
        const lv_coord_t x =
            left ? HOME_SLASH_SIDE_PAD
                 : (HOME_SLASH_SIDE_PAD + cell_w - HOME_SLASH_SLANT + HOME_SLASH_COL_GAP);

        lv_obj_t *cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cell_w, row_h);
        lv_obj_set_pos(cell, x, 0);
        // 不用矩形 bg/border，外形完全由 Home_OnSlashCellDraw 梯形决定
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        // 图标+文字成组靠外侧，勿 SPACE_BETWEEN 把字顶到中缝
        if (left)
        {
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_left(cell, HOME_SLASH_PAD_OUTER, 0);
            lv_obj_set_style_pad_right(cell, HOME_SLASH_SLANT + HOME_SLASH_PAD_SLANT, 0);
            lv_obj_set_style_pad_column(cell, HOME_SLASH_PAD_COL, 0);
        }
        else
        {
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_left(cell, HOME_SLASH_SLANT + HOME_SLASH_PAD_SLANT, 0);
            // 右卡文字略贴图标、整体更靠右（如「每日清单」）
            lv_obj_set_style_pad_right(cell, HOME_SLASH_PAD_OUTER - 6, 0);
            lv_obj_set_style_pad_column(cell, HOME_SLASH_PAD_COL - HOME_SLASH_NAME_NUDGE_R / 2, 0);
        }
        lv_obj_set_style_pad_top(cell, HOME_SLASH_PAD_V, 0);
        lv_obj_set_style_pad_bottom(cell, HOME_SLASH_PAD_V, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cell);
        lv_obj_set_user_data(cell, const_cast<AppEntry *>(&entry));
        lv_obj_add_event_cb(cell, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry *>(&entry));
        lv_obj_add_event_cb(cell, Home_OnSlashCellDraw, LV_EVENT_DRAW_MAIN,
                            reinterpret_cast<void *>(flags));

        const lv_color_t fg = black ? lv_color_white() : lv_color_black();

        auto make_icon = [&]() {
            lv_obj_t *icon = lv_image_create(cell);
            lv_image_set_src(icon, entry.icon_path);
            lv_obj_set_size(icon, HOME_SLASH_ICON, HOME_SLASH_ICON);
            lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
            if (black)
            {
                lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
                lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            }
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        };
        auto make_name = [&]() {
            lv_obj_t *name = lv_label_create(cell);
            lv_label_set_text(name, entry.name());
            lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
            const lv_font_t *name_font = fontpack_lv_font_get(30, 2);
            if (name_font == nullptr)
            {
                name_font = fontpack_lv_font_ui();
            }
            lv_obj_set_style_text_font(name, name_font, 0);
            lv_obj_set_style_text_color(name, fg, 0);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        };

        if (left)
        {
            make_icon();
            make_name();
        }
        else
        {
            make_name();
            make_icon();
        }
    }

