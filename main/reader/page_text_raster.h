#pragma once

#include "reader_types.h"

#include <lvgl.h>

namespace reader {

/**
 * @brief 将已分页的正文页画成 L8 位图（白 0xFF / 黑 0x00）
 * @note 按行绘制（PageItem 已是折行结果）；含插图的页请勿调用，走原 LVGL 路径。
 * @param page_h 输出高度上限；调用方应先 EstimatePageTextHeight，避免整屏空白 L8 被 LVGL 逐像素 blend
 * @param underline_mode kBookReaderUnderline* 同名取值（0/1/2），本头不依赖 book UI
 */
bool RasterPageTextToL8(const Page& page, const lv_font_t* font, lv_coord_t line_gap,
                        lv_coord_t para_gap, int page_w, int page_h, int underline_mode,
                        RasterImage& out);

/**
 * @brief 估算正文墨迹高度（不含大片空白），供裁剪 L8 高度
 * @return 至少一行高；不超过 max_h
 */
int EstimatePageTextHeight(const Page& page, const lv_font_t* font, lv_coord_t line_gap,
                           lv_coord_t para_gap, int max_h);

/** @brief 页内是否仅有文本（可走页光栅） */
bool PageIsTextOnly(const Page& page);

}  // namespace reader
