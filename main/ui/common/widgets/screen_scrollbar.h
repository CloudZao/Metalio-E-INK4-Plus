#pragma once

#include <lvgl.h>

/** @brief 为自行绘制的分页滚动条接入统一显示时限，重复调用会重新显示。 */
void ScreenScrollbarAttach(lv_obj_t* viewport);

/** @brief 翻页或滚动后重新显示滚动条，三秒无操作后隐藏。 */
void ScreenScrollbarShow(lv_obj_t* viewport);

/** @brief 查询自绘滚动条是否处于显示时段；未能分配状态时保持显示。 */
bool ScreenScrollbarVisible(lv_obj_t* viewport);

/** @brief 在 UI 任务中发现当前页面的原生滚动条并处理统一隐藏时限。 */
void ScreenScrollbarPoll();
