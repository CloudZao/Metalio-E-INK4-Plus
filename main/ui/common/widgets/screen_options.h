#pragma once
#include <cstdint>
#include <lvgl.h>
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/drawing/ui_scale.h"

constexpr lv_coord_t kScreenProgressSliderH = UiSy(16); // 阅读与前光进度轨道高度
constexpr lv_coord_t kScreenProgressKnobSize = UiSx(30); // 进度圆形滑块的目标直径
constexpr lv_coord_t kScreenOptionH = UiSy(48);    // 可选中选项行高
constexpr lv_coord_t kScreenOptionGap = UiSy(12);  // 选项间距
constexpr lv_coord_t kScreenBorderW = UiSx(2);     // 描边宽度
constexpr lv_coord_t kScreenOptionRadius = UiSx(8);
constexpr lv_coord_t kScreenOptionPad = UiSx(8);

/** @brief 设置阅读进度与前光共用的描边轨道和圆形滑块。 */
void ScreenStyleProgressSlider(lv_obj_t* slider);

/** @brief 按当前明暗模式设置选中配色及系统字体字重，不改变调用方布局。 */
void ScreenStyleSelectable(lv_obj_t* button, lv_obj_t* label, bool selected);

/** @brief 创建按可用宽度和系统字体分列的选项组。 */
lv_obj_t* ScreenCreateOptionGrid(lv_obj_t* parent, int maximum_columns);

/** @brief 创建使用本机系统字体的选项按钮，尺寸和事件由所属页面管理。 */
lv_obj_t* ScreenCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t callback, intptr_t user_data);
