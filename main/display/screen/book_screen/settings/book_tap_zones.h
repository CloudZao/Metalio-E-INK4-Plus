#pragma once

#include <cstdint>

// 正文 3×3 触摸分区（NVS namespace "reader" / key "tap_zones"）：
// 行主序 cell 0..8；默认左列上一页、中列菜单、右列下一页。
// 动作：无 / 上一页 / 菜单 / 下一页 / 返回 / 目录 / 全刷一次。
// 读走 RAM 缓存；首次 Ensure 须在 DRAM 栈；落盘在内部 RAM 栈任务。

enum class BookTapZoneAction : uint8_t {
    kNone = 0,
    kPrev = 1,
    kMenu = 2,
    kNext = 3,
    kBack = 4,
    kToc = 5,
    kFullRefresh = 6, // 下一帧 EPD 全刷一次
    kCount = 7,
};

constexpr int kBookTapZoneCellCount = 9;

/** @brief 确保触摸分区配置已加载 */
void BookTapZonesEnsureLoaded();

/** @brief 取某格动作 */
BookTapZoneAction BookTapZonesActionAt(int cell);
/** @brief 设置某格动作并落盘 */
void BookTapZonesSetAction(int cell, BookTapZoneAction action);
/** @brief 恢复分区默认并落盘 */
void BookTapZonesResetDefaults();

/** @brief 屏坐标命中格；w/h<=0 返回 -1。右/下开区间：col = min(2, x*3/w)。 */
int BookTapZonesHit(int x, int y, int w, int h);
