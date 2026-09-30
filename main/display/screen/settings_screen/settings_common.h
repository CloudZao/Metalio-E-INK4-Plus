#pragma once

#include "lvgl.h"

#include "fontpack_lvgl.h"
#include "ui_scale.h"

#include <cstdint>

// 源值对齐 397 settings_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kSettingsBodyPad = UiSx(10); // 设置页内容区内边距
constexpr lv_coord_t kSettingsTabW = UiSx(120); // 侧栏 Tab 宽
constexpr lv_coord_t kSettingsTabH = UiSy(56); // 侧栏 Tab 高
constexpr lv_coord_t kSettingsTabGap = UiSy(8); // Tab 间距
constexpr lv_coord_t kSettingsSplitLineW = UiSx(2); // 分区分割线宽
constexpr lv_coord_t kSettingsContentPad = UiSx(14); // 右侧内容区内边距
constexpr lv_coord_t kSettingsOptionH = UiSy(64); // 可选中选项行高
constexpr lv_coord_t kSettingsOptionGap = UiSy(12); // 选项间距
constexpr lv_coord_t kSettingsBorderW = UiSx(2); // 描边宽度
constexpr lv_coord_t kSettingsOptionRadius = UiSx(8);
constexpr lv_coord_t kSettingsOptionPad = UiSx(10);
constexpr lv_coord_t kSettingsBrightBtnW = UiSx(96);
constexpr lv_coord_t kSettingsInfoRowH = UiSy(72);
constexpr lv_coord_t kSettingsInfoRowGap = UiSy(8);
constexpr lv_coord_t kSettingsInfoRowPadH = UiSx(12);
constexpr lv_coord_t kSettingsInfoRowPadV = UiSy(8);

/** @brief 可选中按钮外观：选中黑底白字 / 未选中白底黑字描边 */
void SettingsStyleSelectable(lv_obj_t* btn, lv_obj_t* lbl, bool selected);

/** @brief 创建整行可选中选项 */
lv_obj_t* SettingsCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t cb,
                                         intptr_t user_data);
