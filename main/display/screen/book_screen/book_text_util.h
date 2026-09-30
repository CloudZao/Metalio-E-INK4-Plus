#pragma once

#include <cstdint>
#include <string>

#include <lvgl.h>

#include "ui_scale.h"

/** @brief 内容区可用宽度 */
lv_coord_t Book_ContentWidth();
/** @brief 按字体测量文本像素宽 */
lv_coord_t Book_MeasureTextWidth(const lv_font_t* font, const char* text);
/** @brief 单行截断：过长则末尾加「…」，按像素宽度预算。 */
std::string TruncateTextToWidth(const std::string& text, const lv_font_t* font, lv_coord_t max_w);
/** @brief 文件名单行省略：保留后缀（.ext 或文件夹末尾 /），只截主体。 */
std::string TruncateFilenameKeepExt(const std::string& name, const lv_font_t* font, lv_coord_t max_w);
/** @brief 禁用滚动条与滑动 */
void Book_DisableScroll(lv_obj_t* obj);
constexpr lv_coord_t kColRuleW = UiSx(3);      // 横屏分栏中缝粗竖线
constexpr lv_coord_t kColRuleInset = UiSy(18); // 上下留缝，不把两栏完全隔开
constexpr lv_coord_t kProgressBadgeH = UiSy(28); // 封面进度角标高
constexpr lv_coord_t kProgressBadgeW = UiSx(56); // 封面进度角标宽
/** @brief 横屏分栏中缝：粗竖线，上下留缝不把两栏完全隔开 */
lv_obj_t* Book_AddColRule(lv_obj_t* row);
/** @brief 按左右栏高度收中缝（须在两栏都建完后调用） */
void Book_FinishColRule(lv_obj_t* row, lv_obj_t* host);
/** @brief 在 parent 上居中提示文案 */
void Book_ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font = nullptr);
/** @brief 书架列表总页数 */
int Book_ListPageCount();
/** @brief 钳制书架当前页索引 */
void Book_ClampListPage();

/**
 * @brief 展示用进度（progress_x10）；缓存无效时按 0，不碰 SD
 */
int Book_CachedProgressX10OrZero(const char* book_abs_path);
/**
 * @brief 封面白底黑字进度框（18 号数字 + 手绘 %）
 */
lv_obj_t* Book_CreateProgressPctBadge(lv_obj_t* parent, int pct_x10);
