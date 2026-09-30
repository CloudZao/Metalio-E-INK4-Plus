#pragma once

#include <cstdint>
#include <vector>

#include <lvgl.h>

#include "reader/reader.h"
#include "ui_scale.h"

// 源值对齐 397 book_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kHomeShelfBtnH = UiSy(76);  // 「我的书架」按钮
constexpr lv_coord_t kHomeShelfBtnRadius = UiSx(8); // 方框略圆，勿全胶囊
constexpr lv_coord_t kHomeCardRadius = UiSx(14);
constexpr lv_coord_t kHomeGap = UiSx(12);
constexpr lv_coord_t kHomeSectionGap = UiSy(12); // 继续阅读 ↔ 最近阅读
constexpr lv_coord_t kHomeShelfTopGap = UiSy(22); // 封面 ↔ 我的书架
constexpr lv_coord_t kHomeBottomPad = UiSy(8);   // 书架距屏底再上抬
constexpr lv_coord_t kHomeStatsPad = UiSy(8);    // 统计区上下内边距
constexpr lv_coord_t kHomeStatsGap = UiSy(8);    // 统计行距
constexpr lv_coord_t kRecentCoverH = UiSy(290);  // 最近阅读封面高度上限（比例适中，勿吃满余高）
constexpr lv_coord_t kRecentTitleGap = UiSy(14); // 最近阅读封面与书名间距
constexpr lv_coord_t kHomeChartBarW = UiSx(12); // 阅读数据柱状图竖条宽
constexpr lv_coord_t kContPad = UiSx(20);       // 继续阅读黑卡内边距
constexpr lv_coord_t kContBtnH = UiSy(52);      // 继续阅读按钮高
constexpr lv_coord_t kHomeProgBarH = UiSy(16);  // 进度条轨道高
constexpr lv_coord_t kHomeThumbW = UiSx(22);    // 书签占位宽
constexpr lv_coord_t kHomeDashInnerGap = UiSy(16); // 继续阅读卡内纵距

struct HomeStats {
    int continue_index = -1;     // 继续阅读书目下标；-1 无
    int continue_progress_x10 = -1;
    uint32_t total_seconds = 0;
    int finished_count = 0;
    int recent_indices[2] = {-1, -1};
    bool has_recent = false;  // NVS MRU 非空且能对上当前书库
};

/** @brief 渲染阅读应用首页 */
void RenderReadingHome();
/**
 * @brief 汇总首页统计（继续阅读/时长/已看完/最近）
 * @note 不再全库 Peek .pos；最近/继续走 book_home_snapshot MRU
 */
HomeStats CollectHomeStats(const std::vector<reader::BookInfo>& books);
/** @brief 首页时长短文案（如 12m / 1h） */
void FormatHomeDurationShort(char* out, size_t out_sz, uint32_t seconds);
/** @brief 首页继续阅读卡进度文案；100% 时不带小数 */
void FormatBookReadPctLabel(char* out, size_t out_sz, int pct_x10);
/** @brief 「继续阅读」点击 */
void OnContinueReadingClicked(lv_event_t* e);
/** @brief 「我的书架」点击 */
void OnOpenShelfClicked(lv_event_t* e);
/** @brief 异步回到阅读首页 */
void BackToReadingHomeAsync(void* user_data);
/** @brief 异步回到书架/书库 */
void BackToLibraryAsync(void* user_data);
