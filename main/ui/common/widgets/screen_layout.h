#pragma once

#include <lvgl.h>
#include "ui/common/drawing/ui_layout.h"

/**
 * @brief 完成当前布局后取得父容器逻辑内容尺寸，不重复扣除内边距；隐藏容器可能为零。
 * @note 须在LVGL线程或持有UI锁时调用。
 */
static inline UiSize ScreenContentSize(lv_obj_t* parent) {
    lv_obj_update_layout(parent);
    return {static_cast<int>(lv_obj_get_content_width(parent)), static_cast<int>(lv_obj_get_content_height(parent))};
}
