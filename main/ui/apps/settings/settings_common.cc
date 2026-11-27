#include <algorithm>
#include <cstring>
#include "assets/lang_config.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/drawing/font/font_awesome.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/screen_gesture.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_scrollbar.h"

LV_FONT_DECLARE(font_awesome_30_1);

static void CenterSettingsRowText(lv_obj_t* label) {
    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (font == &font_awesome_30_1) {
        return;
    }
    lv_font_glyph_dsc_t glyph{};
    const uint32_t letter = std::strcmp(LangCode, "zh-CN") == 0 ? 0x4e2d : 'H';
    if (lv_font_get_glyph_dsc(font, &glyph, letter, 0) && glyph.box_h > 0) {
        const int offset = font->base_line + glyph.ofs_y + (static_cast<int>(glyph.box_h) - font->line_height) / 2;
        if (lv_obj_get_style_translate_y(label, LV_PART_MAIN) != offset) {
            lv_obj_set_style_translate_y(label, offset, 0);
        }
    }
}

static int MeasureSettingsRowTextWidth(lv_obj_t* label) {
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void LayoutSettingsListRow(lv_event_t* event) {
    auto* row = lv_event_get_current_target_obj(event);
    lv_obj_t* title = nullptr;
    lv_obj_t* detail = nullptr;
    int available = lv_obj_get_content_width(row);
    int visible = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        auto* child = lv_obj_get_child(row, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        ++visible;
        if (lv_obj_check_type(child, &lv_label_class)) {
            if (lv_obj_get_style_flex_grow(child, LV_PART_MAIN) > 0) {
                title = child;
            } else if (title && !detail && lv_obj_get_style_text_font(child, LV_PART_MAIN) != &font_awesome_30_1) {
                detail = child;
            }
        }
    }
    if (title && detail) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
            auto* child = lv_obj_get_child(row, i);
            if (child != title && child != detail && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                available -= lv_obj_get_width(child);
            }
        }
        available -= (visible - 1) * lv_obj_get_style_pad_column(row, LV_PART_MAIN);
        available = std::max(1, available);
        // 保留标题所需宽度；两侧都较长时沿用原比例，短标题让出空余空间。
        const int title_width = std::min(MeasureSettingsRowTextWidth(title), available * 54 / 100);
        const int detail_max_width = std::max(1, available - title_width);
        if (lv_obj_get_style_max_width(detail, LV_PART_MAIN) != detail_max_width) {
            lv_obj_set_style_max_width(detail, detail_max_width, 0);
        }
    }
    int content_height = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        auto* child = lv_obj_get_child(row, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        content_height = std::max<int>(content_height, lv_obj_get_height(child));
        if (lv_obj_check_type(child, &lv_label_class)) {
            CenterSettingsRowText(child);
        }
    }
    const int inset = lv_obj_get_height(row) - lv_obj_get_content_height(row);
    const int height = std::max<int>(lv_obj_get_style_min_height(row, LV_PART_MAIN), content_height + inset);
    if (lv_obj_get_style_height(row, LV_PART_MAIN) != height) {
        lv_obj_set_height(row, height);
    }
}

static bool TextCrossesEdge(lv_obj_t* object, int edge) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        const auto* font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        const int stride = font->line_height + lv_obj_get_style_text_line_space(object, LV_PART_MAIN);
        for (int top = area.y1; top <= area.y2; top += std::max(1, stride)) {
            if (edge > top && edge < top + font->line_height) {
                return true;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        if (TextCrossesEdge(lv_obj_get_child(object, i), edge)) {
            return true;
        }
    }
    return false;
}

static int LastSettingsScrollStop(lv_obj_t* page) {
    lv_area_t area;
    lv_obj_get_coords(page, &area);
    const int current = lv_obj_get_scroll_y(page);
    const int maximum = current + lv_obj_get_scroll_bottom(page);
    for (int candidate = maximum; candidate >= std::max<int>(0, maximum - lv_area_get_height(&area)); --candidate) {
        const int delta = candidate - current;
        if (!TextCrossesEdge(page, area.y1 + delta) && !TextCrossesEdge(page, area.y2 + delta)) {
            return candidate;
        }
    }
    return 0;
}

static void PageGesture(lv_event_t* event) {
    auto* input = lv_indev_active();
    if (!input) {
        return;
    }
    const auto direction = lv_indev_get_gesture_dir(input);
    if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM) {
        return;
    }
    auto* page = lv_event_get_current_target_obj(event);
    ScreenScrollbarShow(page);
    if (SettingsScrollPage(page, direction == LV_DIR_TOP ? 1 : -1)) {
        lv_event_stop_processing(event);
        lv_event_stop_bubbling(event);
        lv_indev_wait_release(input);
    }
}

static void DrawSettingsScrollbar(lv_event_t* event) {
    auto* page = lv_event_get_current_target_obj(event);
    if (!ScreenScrollbarVisible(page)) {
        return;
    }
    const int current = lv_obj_get_scroll_y(page);
    const int maximum = LastSettingsScrollStop(page);
    if (maximum <= 0) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(page, &area);
    const int track = lv_area_get_height(&area) - UiSy(8);
    const int length = std::max<int>(UiSy(16), track * track / (track + maximum));
    area.y1 += UiSy(4) + (track - length) * current / maximum;
    area.y2 = area.y1 + length - 1;
    area.x2 -= UiSx(2) + 2;
    area.x1 = area.x2 - UiSx(3) + 1;
    lv_draw_rect_dsc_t style;
    lv_draw_rect_dsc_init(&style);
    style.bg_color = UiColorForeground();
    style.bg_opa = LV_OPA_COVER;
    style.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(lv_event_get_layer(event), &style, &area);
}

static void LayoutSettingsOptions(lv_event_t* event) {
    auto* grid = lv_event_get_current_target_obj(event);
    const int requested = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int count = lv_obj_get_child_count(grid);
    const int available = lv_obj_get_content_width(grid);
    if (available <= 0 || count == 0) {
        return;
    }
    const int options = requested < 0 ? -requested : count;
    int columns = requested > 0 ? requested : (options % 2 ? 3 : 2);
    columns = std::min(columns, options);
    const int gap = lv_obj_get_style_pad_column(grid, LV_PART_MAIN);
    if (requested <= 0) {
        int widest = 0;
        for (int i = 0; i < count; ++i) {
            auto* button = lv_obj_get_child(grid, i);
            if (lv_obj_get_child_count(button) == 0) {
                continue;
            }
            auto* label = lv_obj_get_child(button, 0);
            widest = std::max<int>(widest, ScreenMeasureTextWidth(UiFont(), lv_label_get_text(label)));
        }
        if ((available - gap * (columns - 1)) / columns < widest + 2 * (kScreenOptionPad + kScreenBorderW)) {
            columns = 1;
        }
    }
    const int width = std::max(1, (available - gap * (columns - 1)) / columns);
    for (int i = 0; i < count; ++i) {
        auto* button = lv_obj_get_child(grid, i);
        if (lv_obj_get_style_width(button, LV_PART_MAIN) != width) {
            lv_obj_set_width(button, width);
        }
    }
}

static void LayoutSettingsListGroup(lv_event_t* event) {
    auto* group = lv_event_get_current_target_obj(event);
    const uint32_t count = lv_obj_get_child_count(group);
    for (uint32_t i = 0; i < count; ++i) {
        auto* row = lv_obj_get_child(group, i);
        const int border = i + 1 < count ? 1 : 0;
        if (lv_obj_get_style_border_width(row, LV_PART_MAIN) != border) {
            lv_obj_set_style_border_width(row, border, 0);
        }
    }
}

void SettingsStyleBrightnessButton(lv_obj_t* button) {
    lv_obj_set_size(button, kSettingsBrightnessButtonW, kSettingsBrightnessButtonH);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, kScreenOptionRadius, 0);
    lv_obj_set_style_border_width(button, kScreenBorderW, 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    auto* label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_center(label);
}

void SettingsCreateSectionTitle(lv_obj_t* page, const char* text) {
    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, text);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_set_style_pad_left(title, UiSx(8), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

void SettingsCreateSectionHint(lv_obj_t* page, const char* text) {
    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, text);
    lv_obj_set_style_text_font(hint, UiFont(UiFontRole::Caption), 0);
    lv_obj_set_style_text_color(hint, UiColorForeground(), 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_pad_hor(hint, UiSx(8), 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* SettingsCreateSection(lv_obj_t* page, const char* title) {
    auto* section = lv_obj_create(page);
    lv_obj_remove_style_all(section);
    lv_obj_set_size(section, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(section, UiSy(5), 0);
    lv_obj_clear_flag(section, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (title) {
        SettingsCreateSectionTitle(section, title);
    }
    return section;
}

lv_obj_t* SettingsCreateListRow(lv_obj_t* parent, const char* title, const char* value,
                               lv_event_cb_t callback, intptr_t data, bool show_arrow) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // 内容高度会使 flex 跨轴对齐退化为顶部对齐；显式行高由布局回调随换行调整。
    lv_obj_set_size(row, lv_pct(100), UiSy(51));
    lv_obj_set_style_min_height(row, UiSy(51), 0);
    lv_obj_set_style_pad_hor(row, UiSx(11), 0);
    lv_obj_set_style_pad_ver(row, UiSy(9), 0);
    lv_obj_set_style_pad_column(row, UiSx(10), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, LayoutSettingsListRow, LV_EVENT_LAYOUT_CHANGED, nullptr);
    if (callback) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, reinterpret_cast<void*>(data));
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    auto* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    if (value && (value[0] || !show_arrow)) {
        auto* detail = lv_label_create(row);
        lv_label_set_text(detail, value);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(detail, UiFont(UiFontRole::Caption), 0);
    }
    if (callback && show_arrow) {
        auto* arrow = lv_label_create(row);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(arrow, &font_awesome_30_1, 0);
    }
    return row;
}

void SettingsSetListRowCurrent(lv_obj_t* row, bool current) {
    if (!row) {
        return;
    }
    auto* title = lv_obj_get_child(row, 0);
    auto* value = lv_obj_get_child(row, 1);
    const auto* title_font = UiFont(current ? UiFontRole::Emphasis : UiFontRole::Body);
    if (lv_obj_get_style_text_font(title, LV_PART_MAIN) != title_font) {
        lv_obj_set_style_text_font(title, title_font, 0);
    }
    if (lv_obj_check_type(value, &lv_label_class)) {
        // 字体预览入口保留箭头，其余选项替换原有状态标签。
        if (lv_obj_get_style_text_font(value, LV_PART_MAIN) != &font_awesome_30_1) {
            lv_obj_delete(value);
        }
        value = lv_obj_create(row);
        lv_obj_move_to_index(value, 1);
        lv_obj_remove_style_all(value);
        lv_obj_set_size(value, UiSx(22), UiSx(22));
        lv_obj_set_style_radius(value, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(value, kScreenBorderW, 0);
        lv_obj_set_style_border_color(value, UiColorForeground(), 0);
        lv_obj_clear_flag(value, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        auto* dot = lv_obj_create(value);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, UiSx(10), UiSx(10));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_center(dot);
    }
    lv_obj_set_flag(value, LV_OBJ_FLAG_HIDDEN, !current);
}

bool SettingsScrollPage(lv_obj_t* page, int direction) {
    ScreenScrollbarShow(page);
    if (!page || lv_obj_get_scroll_top(page) + lv_obj_get_scroll_bottom(page) <= 0) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(page, &area);
    const int current = lv_obj_get_scroll_y(page);
    const int maximum = LastSettingsScrollStop(page);
    const int step = lv_area_get_height(&area) * 9 / 10;
    int target = std::clamp(current + (direction > 0 ? step : -step), 0, maximum);
    // 控件边框可以露出半行；停靠边界须避开文字行。
    if (target == current) {
        return false;
    }
    const int preferred = target;
    bool found = false;
    for (int distance = 0; distance < lv_area_get_height(&area); ++distance) {
        for (int candidate : {preferred - distance, preferred + distance}) {
            if (candidate < 0 || candidate > maximum || (direction > 0 ? candidate <= current : candidate >= current)) {
                continue;
            }
            const int delta = candidate - current;
            if (!TextCrossesEdge(page, area.y1 + delta) && !TextCrossesEdge(page, area.y2 + delta)) {
                target = candidate;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        return false;
    }
    lv_obj_scroll_to_y(page, target, LV_ANIM_OFF);
    lv_obj_invalidate(page);
    return true;
}

void SettingsAttachPagedScroll(lv_obj_t* page) {
    lv_obj_clear_flag(page, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                                                     LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(page, PageGesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(page, DrawSettingsScrollbar, LV_EVENT_DRAW_POST_END, nullptr);
    ScreenScrollbarAttach(page);
    lv_obj_add_event_cb(page, [](lv_event_t* event) {
        auto* page = lv_event_get_current_target_obj(event);
        const int full_height = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        if (lv_obj_get_scroll_y(page) == 0 && full_height > 0) {
            lv_area_t area;
            lv_obj_get_coords(page, &area);
            for (int inset = 0; inset <= ScreenTextHeight(); ++inset) {
                if (!TextCrossesEdge(page, area.y1 + full_height - inset - 1)) {
                    if (lv_obj_get_style_height(page, LV_PART_MAIN) != full_height - inset) {
                        lv_obj_set_height(page, full_height - inset);
                    }
                    break;
                }
            }
        }
        if (lv_obj_get_scroll_top(page) + lv_obj_get_scroll_bottom(page) > 0) {
            lv_obj_add_flag(page, kTouchUiPagedVerticalDrag);
        } else {
            lv_obj_clear_flag(page, kTouchUiPagedVerticalDrag);
        }
    }, LV_EVENT_LAYOUT_CHANGED, reinterpret_cast<void*>(static_cast<intptr_t>(lv_obj_get_style_height(page, LV_PART_MAIN))));
}

lv_obj_t* SettingsCreateOptionGrid(lv_obj_t* parent, int count, int columns) {
    auto* grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, UiSx(11), 0);
    lv_obj_set_style_pad_row(grid, UiSx(11), 0);
    lv_obj_clear_flag(grid, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* data = reinterpret_cast<void*>(static_cast<intptr_t>(columns > 0 ? columns : -(count > 0 ? count : 1)));
    lv_obj_add_event_cb(grid, LayoutSettingsOptions, LV_EVENT_SIZE_CHANGED, data);
    lv_obj_add_event_cb(grid, LayoutSettingsOptions, LV_EVENT_CHILD_CHANGED, data);
    return grid;
}

lv_obj_t* SettingsCreateToggle(lv_obj_t* parent, const char* title, lv_event_cb_t callback) {
    auto* row = SettingsCreateListRow(parent, title, "");
    auto* toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, UiSx(49), UiSy(27));
    lv_obj_set_style_bg_color(toggle, UiColorBackground(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle, kScreenBorderW, LV_PART_MAIN);
    lv_obj_set_style_border_color(toggle, UiColorForeground(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(toggle, UiColorBackground(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(toggle, UiColorForeground(), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, UiColorForeground(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(toggle, UiColorBackground(), static_cast<lv_style_selector_t>(LV_PART_KNOB) | LV_STATE_CHECKED);
    lv_obj_clear_flag(toggle, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(toggle);
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, nullptr);
    return toggle;
}

lv_obj_t* SettingsCreateListGroup(lv_obj_t* page) {
    auto* group = lv_obj_create(page);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(group, kScreenBorderW, 0);
    lv_obj_set_style_border_color(group, UiColorForeground(), 0);
    lv_obj_set_style_radius(group, UiSx(14), 0);
    lv_obj_set_style_border_opa(group, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_clear_flag(group, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_event_cb(group, LayoutSettingsListGroup, LV_EVENT_LAYOUT_CHANGED, nullptr);
    return group;
}

lv_obj_t* SettingsCreateBackButton(lv_obj_t* parent, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, UiSx(48), UiSy(48));
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(button);
    auto* arrow = lv_label_create(button);
    lv_label_set_text(arrow, FONT_AWESOME_ANGLE_LEFT);
    lv_obj_set_style_text_font(arrow, &font_awesome_30_1, 0);
    lv_obj_center(arrow);
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    }
    return button;
}
