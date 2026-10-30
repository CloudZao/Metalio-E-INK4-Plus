#pragma once

#include "reader_types.h"

#include <lvgl.h>

namespace reader {

inline constexpr int kReadingBackgroundPitch = 32; // 方格目标间距与点阵间距，单位像素

/**
 * @brief 计算阅读网格或点阵背景像素；正文与背景层共用相同坐标规则
 * @note 方格按固定内容区均分，边格与内格等大，像素取整误差不超过 1。
 */
uint8_t ReadingBackgroundPixel(int mode, int x, int y, int left, int width, int height);

/**
 * @brief 将已分页的正文页画成 L8 位图（白 0xFF / 黑 0x00）
 * @note 按行绘制（PageItem 已是折行结果）；含插图的页请勿调用，走原 LVGL 路径。
 * @param page_h 完整阅读视口高度；内部按实际占用裁剪输出，保留完整视口的行布局
 * @param underline_mode kBookReaderUnderline* 同名取值（0/1/2），本头不依赖 book UI
 */
bool RasterPageTextToL8(const Page& page, const lv_font_t* font, int spacing_percent,
                        lv_coord_t para_gap, int page_w, int page_h, int underline_mode,
                        RasterImage& out, bool antialias = false);

/**
 * @brief 将竖排正文画成 L8 位图，按文字方向排列各列
 * @param right_to_left 为真时列从右往左排列；下划线始终位于列右侧
 */
bool RasterPageVerticalTextToL8(const Page& page, const lv_font_t* font, int spacing_percent,
                                lv_coord_t para_gap, int page_w, int page_h, int rule_mode,
                                RasterImage& out, bool antialias = false, bool right_to_left = false);

/**
 * @brief 按完整视口的共享行单元计算已用高度，供裁剪短页输出
 * @return 返回已用高度；页面不适配当前行布局或没有正文时返回 0
 */
int EstimatePageTextHeight(const Page& page, const lv_font_t* font, int spacing_percent,
                           lv_coord_t para_gap, int max_h);

/** @brief 页内是否仅有文本（可走页光栅） */
bool PageIsTextOnly(const Page& page);

}  // namespace reader
