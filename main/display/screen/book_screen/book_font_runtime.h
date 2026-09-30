#pragma once

#include <lvgl.h>

#include "reader/reader_types.h"

/** @brief 书库/详情等界面标题：固件 fontpack 30@2 */
const lv_font_t* Book_ListFont();
/** @brief 书库列表 item：固件 fontpack UI 默认字（30@2） */
const lv_font_t* Book_ItemFont();
/** @brief 仅正文内容：SD epdfont（NVS 所选），失败回退 fontpack UI */
const lv_font_t* BookFont();
/** @brief 按 NVS 确保正文 TTF/fontpack 已加载 */
void EnsureBookFont();
/** @brief 释放正文动态字体资源 */
void ReleaseBookFont();
/** @brief 预热当前页用到的字形 */
void PrewarmPageFont(const reader::Page* page);
