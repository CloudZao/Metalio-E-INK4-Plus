#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace reader {
namespace book_home_snapshot {

/** 最近阅读 MRU 上限（首页展示前 2 本；NVS 保留 4 本供轮换） */
constexpr size_t kRecentMax = 4;
/** 首页「最近阅读」格子数 */
constexpr size_t kRecentDisplayMax = 2;

/**
 * 阅读首页快照（与书库列表缓存正交）：
 * - 最近阅读：NVS MRU（相对 books/ 的路径），仅在真正写进度时轮换；空则不展示
 * - 聚合统计：保留已有摘要，单本进度成功保存后增量更新，首页不遍历全库 .pos
 *
 * Flash/NVS 禁忌：禁止在 LVGL（栈常在 PSRAM）路径直接 nvs_*。
 * 必须先由 DRAM 栈任务 HydrateFromNvs()；UI 只碰内存；落盘走异步 DRAM 任务。
 */

struct WeekStats {
    bool valid = false; // local date trusted and NVS hydrated
    uint32_t dates[7] = {}; // Monday through Sunday
    uint32_t seconds[7] = {};
    uint64_t total_seconds = 0;
    uint32_t undated_seconds = 0; // retained until a trusted calendar date becomes available
};

/** Add only the newly folded seconds; shares the existing NVS worker. */
void AddDailyReadingSeconds(uint32_t date, uint32_t seconds);
/** Memory-only current calendar week; no UI-side NVS access. */
WeekStats LoadWeekStats();

struct AggregateStats {
    bool valid = false;
    uint32_t total_seconds = 0;
    int finished_count = 0;
};

/**
 * 从 NVS 灌入内存（幂等）。
 * 仅允许在内部 RAM 栈任务调用（main_task / book_prog_sync 等），禁止 LVGL 调用。
 */
void HydrateFromNvs();

/** 成功 SaveProgress 后调用：将该书置为 MRU 头；已在队首则跳过。落盘异步。 */
void NoteOpenedBook(const char* book_abs_path);

/** 删书后调用：从 MRU 去掉该路径。落盘异步。 */
void RemoveBook(const char* book_abs_path);

/**
 * 删书时从聚合统计扣减该书贡献（须在 unlink .pos 之前 Peek 得到）。
 * 无有效快照或 seconds/finished 为 0 时为无操作。落盘异步。
 */
void SubtractAggregateContribution(uint32_t reading_seconds, bool finished);

/**
 * 读出最近阅读绝对路径（仅内存；未 Hydrate 则空）。
 * 不访问 SD，文件存在性在打开时由后台检查。
 */
size_t LoadRecentAbsPaths(std::vector<std::string>& out);

/** @brief 仅读取内存中的最近阅读路径，不检查磁盘；无内存记录时返回空串 */
std::string LoadLatestAbsPath();

/** 仅读内存；未 Hydrate 则 valid=false。 */
AggregateStats LoadAggregateStats();

/** @brief 单本进度成功保存后增量更新现有统计，不遍历书库 */
void UpdateAggregateProgress(uint32_t old_seconds, bool old_finished, uint32_t seconds, bool finished);

}  // namespace book_home_snapshot
}  // namespace reader
