#pragma once

#include "lvgl.h"

// 首页：状态栏 + Hero + 应用宫格；支持多种卡片样式。
class HomeScreen {
public:
    static constexpr int kCardStyleWhite = 0;
    static constexpr int kCardStyleGray = 1;
    static constexpr int kCardStyleBorder = 2;
    static constexpr int kCardStyleSlash = 3;
    static constexpr int kCardStylePixel = 4; // 像素视界

    /** @brief 创建首页 */
    static lv_obj_t* Create();

    /** @brief 异步返回首页第一页；已在首页时仅切换应用页，投递成功返回 true。 */
    static bool GoToFirstPage();

    /** @brief 读取首页卡片样式；非法值回落到默认 */
    static int LoadCardStyle();
    /** @brief 持久化首页卡片样式 */
    static void SaveCardStyle(int style);
};
