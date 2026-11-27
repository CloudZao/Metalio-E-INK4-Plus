#pragma once

#include <algorithm>
#include <lvgl.h>
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

/** @brief 为禁用按钮绘制圆角虚线边框。 */
static inline void ScreenDrawDisabledBorder(lv_event_t* event) {
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (!lv_obj_has_state(button, LV_STATE_DISABLED)) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    lv_area_increase(&area, -1, -1);
    const int radius = UiSx(8), dash = UiSx(4), pitch = UiSx(8);
    auto* layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = UiColorForeground();
    line.width = 2;
    for (int x = area.x1 + radius; x < area.x2 - radius; x += pitch) {
        line.p1 = {x, area.y1};
        line.p2 = {std::min<int>(x + dash, area.x2 - radius), area.y1};
        lv_draw_line(layer, &line);
        line.p1.y = line.p2.y = area.y2;
        lv_draw_line(layer, &line);
    }
    for (int y = area.y1 + radius; y < area.y2 - radius; y += pitch) {
        line.p1 = {area.x1, y};
        line.p2 = {area.x1, std::min<int>(y + dash, area.y2 - radius)};
        lv_draw_line(layer, &line);
        line.p1.x = line.p2.x = area.x2;
        lv_draw_line(layer, &line);
    }
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = UiColorForeground();
    arc.width = 2;
    arc.radius = radius;
    const lv_point_t centers[] = {{area.x2 - radius, area.y2 - radius},
                                  {area.x1 + radius, area.y2 - radius},
                                  {area.x1 + radius, area.y1 + radius},
                                  {area.x2 - radius, area.y1 + radius}};
    for (int corner = 0; corner < 4; ++corner) {
        for (int angle = 0; angle < 90; angle += 30) {
            arc.center = centers[corner];
            arc.start_angle = corner * 90 + angle;
            arc.end_angle = arc.start_angle + 15;
            lv_draw_arc(layer, &arc);
        }
    }
}
