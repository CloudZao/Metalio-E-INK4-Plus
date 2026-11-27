#pragma once

#include <lvgl.h>

/** @brief 构建系统字体列表，字体选择后进入独立预览页。 */
void SettingsFontTab_Build(lv_obj_t* page);

/** @brief 取消目录请求、释放名称缓存并清除字体页面引用。 */
void SettingsFontTab_Reset();

/** @brief 从字体预览回到列表；不是预览页时返回 false。 */
bool SettingsFontTab_Back();

/** @brief 在字体列表内按视口滚动，边界切换按视口计算的目录批次。 */
bool SettingsFontTab_Scroll(int direction);
