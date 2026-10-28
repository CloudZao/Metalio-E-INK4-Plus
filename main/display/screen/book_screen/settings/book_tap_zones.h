#pragma once

#include <cstdint>

// 正文 3×3 触摸分区（NVS namespace "reader" / key "tap_zones"）：
// 行主序 cell 0..8；默认左列上一页、中列菜单、右列下一页。
// 动作：无 / 上一页 / 菜单 / 下一页 / 返回 / 目录 / 全刷一次 / 书签 / 字体 / 设置。
// 读走 RAM 缓存；首次 Ensure 须在 DRAM 栈；落盘在内部 RAM 栈任务。

// 动作编号保存在 NVS 中，新增动作须追加，已有编号不得变更。
enum class BookTapZoneAction : uint8_t {
    kNone = 0, // 无操作
    kPrev = 1, // 上一页
    kMenu = 2, // 唤醒菜单
    kNext = 3, // 下一页
    kBack = 4, // 返回
    kToc = 5, // 打开目录
    kFullRefresh = 6, // 下一帧 EPD 全刷一次
    kBookmarks = 7, // 打开书签
    kFontSettings = 8, // 打开字体设置
    kSettings = 9, // 打开阅读设置
    kCount = 10, // 动作数量
};

constexpr int kBookTapZoneCellCount = 9;

/** @brief 确保触摸分区配置已加载 */
void BookTapZonesEnsureLoaded();

/** @brief 取某格动作 */
BookTapZoneAction BookTapZonesActionAt(int cell);
/**
 * @brief 更新分区动作并异步请求保存，至少保留一个菜单唤醒区域
 * @param cell 分区索引，范围为 0 至 kBookTapZoneCellCount - 1
 * @param action 分区动作
 * @return 配置被接受时返回 true，参数无效或移除最后一个菜单分区时返回 false
 * @note 返回 true 不表示异步保存已完成
 */
bool BookTapZonesSetAction(int cell, BookTapZoneAction action);
/** @brief 恢复默认分区并异步请求保存 */
void BookTapZonesResetDefaults();

/** @brief 屏坐标命中格；w/h<=0 返回 -1。右/下开区间：col = min(2, x*3/w)。 */
int BookTapZonesHit(int x, int y, int w, int h);
