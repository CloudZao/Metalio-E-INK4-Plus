#pragma once
#include <memory>
#include <lvgl.h>
#include "business/diagnostics/aging_test.h"

/** @brief 创建方形预览区，并返回可用于后台解码的内部尺寸。 */
lv_obj_t* SettingsTestAgingSd_Build(lv_obj_t* parent, lv_coord_t side, int& preview_side);

/** @brief 展示业务持有的只读图片结果，在替换前解除旧像素引用。 */
void SettingsTestAgingSd_Show(const std::shared_ptr<const AgingSdResult>& result);

/** @brief 解除图片引用并释放页面持有的结果，不控制测试执行。 */
void SettingsTestAgingSd_Reset();
