#pragma once

#include <cstdint>
#include "lvgl.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/drawing/ui_scale.h"

// 设置控件使用公共逻辑尺寸，经 UiSx/UiSy 适配板型。
constexpr lv_coord_t kSettingsContentPad = UiSx(14);  // 内容区内边距
constexpr lv_coord_t kSettingsSectionGap = UiSy(17);  // 不同功能分区之间的间距

constexpr lv_coord_t kSettingsBrightnessButtonW = UiSx(51);
constexpr lv_coord_t kSettingsBrightnessButtonH = UiSy(48);
constexpr lv_coord_t kSettingsRoundButtonSize = kScreenProgressKnobSize + UiSx(4); // 略大于进度滑块的圆形操作按钮
constexpr lv_coord_t kSettingsBrightnessSliderGap = UiSx(14);

/** @brief 为已有子标签的亮度按钮设置统一尺寸、边框、主题及字体样式。 */
void SettingsStyleBrightnessButton(lv_obj_t* button);

/** @brief 创建设置分区标题。 */
void SettingsCreateSectionTitle(lv_obj_t* page, const char* text);

/** @brief 创建可自动换行的设置说明。 */
void SettingsCreateSectionHint(lv_obj_t* page, const char* text);

/** @brief 创建功能分区，标题、选项与说明紧凑排列，分区间距由父页统一设置。 */
lv_obj_t* SettingsCreateSection(lv_obj_t* page, const char* title = nullptr);

/**
 * @brief 按实际文字宽度分配左右信息，空间不足时换行，可选点击操作和右箭头
 * @param show_arrow 为 false 时不显示箭头，并保留可动态更新的右侧状态列。
 */
lv_obj_t* SettingsCreateListRow(lv_obj_t* parent, const char* title, const char* value,
                               lv_event_cb_t callback = nullptr, intptr_t data = 0, bool show_arrow = true);

/** @brief 更新列表选项右侧的圆圈实心点选中标记。 */
void SettingsSetListRowCurrent(lv_obj_t* row, bool current);

/** @brief 沿指定方向按视口翻动内容；实际位置变化时返回 true。 */
bool SettingsScrollPage(lv_obj_t* page, int direction);

/** @brief 为设置内容容器配置对称留白、滚动条和按视口翻动的手势。 */
void SettingsAttachPagedScroll(lv_obj_t* page);

/** @brief 按选项数量和文字宽度创建等宽独立方块，指定列数时按该列数排列。 */
lv_obj_t* SettingsCreateOptionGrid(lv_obj_t* parent, int count, int columns = 0);

/** @brief 创建带标签的开关行，返回原生开关控件。 */
lv_obj_t* SettingsCreateToggle(lv_obj_t* parent, const char* title, lv_event_cb_t callback);

/** @brief 创建带圆角边框的列表分组，组内各行保持左右排列。 */
lv_obj_t* SettingsCreateListGroup(lv_obj_t* page);

/** @brief 创建仅含返回图标的导航按钮，位置由所属页管理。 */
lv_obj_t* SettingsCreateBackButton(lv_obj_t* parent, lv_event_cb_t callback);
