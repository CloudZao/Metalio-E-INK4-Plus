#pragma once

#include <lvgl.h>

inline constexpr lv_obj_flag_t kTouchUiPagedVerticalDrag = LV_OBJ_FLAG_USER_2;  // 分页列表独占纵向拖动

/** @brief 判断触点下的页面是否保留纵向拖动；须在LVGL线程或持锁时调用。 */
bool ScreenPageOwnsVerticalDrag(lv_point_t point);

/**
 * @brief 在 UI 任务识别左右滑动，松手后分发底部翻页键的默认功能。
 * @param point 当前触点；方向未明确时回写起点，调用方须使用回写坐标向 LVGL 交付触摸。
 * @param valid 当前触点及起点是否有效；无效按下会忽略至松手。
 * @return true 表示已锁定横向拖动，调用方须向 LVGL 报告松开以取消点击及滚动。
 */
bool ScreenHandleHorizontalSwipe(lv_indev_t* indev, bool pressed, lv_point_t& point,
                                 lv_point_t origin, bool valid);
