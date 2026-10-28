#pragma once

#include <lvgl.h>

/** @brief 构建像素视界首页，复用首页应用入口与内存数据缓存 */
void HomePixel_Build(lv_obj_t* screen, lv_coord_t status_height);
/** @brief 停止像素视界刷新并清除控件引用，可重复调用 */
void HomePixel_Stop();

/** @brief 仅更新像素主题底部应用页，保留上方信息 */
void HomePixel_FillAppsPage();
