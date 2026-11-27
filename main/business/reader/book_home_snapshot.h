#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr size_t kBookHomeSnapshotRecentMax = 4;         // 最近阅读 MRU 上限（首页展示前 2 本；NVS 保留 4 本供轮换）
constexpr size_t kBookHomeSnapshotRecentDisplayMax = 2;  // 首页「最近阅读」格子数

// 阅读首页快照（与书库列表缓存正交）：
// - 最近阅读：NVS MRU（相对 books/ 的路径），仅在真正写进度时轮换；空则不展示
// - 聚合统计：保留已有摘要，进度快照交付后增量更新，首页不遍历全库 .pos
// Flash/NVS 禁忌：禁止在 LVGL（栈常在 PSRAM）路径直接 nvs_*。
// 必须先由 DRAM 栈任务 BookHomeSnapshotHydrateFromNvs()；UI 只碰内存；落盘走异步 DRAM 任务。

struct BookHomeSnapshotWeekStats {
    bool valid = false;            // 日期有效且已加载 NVS 数据
    uint32_t dates[7] = {};        // 按周一至周日排列
    uint32_t seconds[7] = {};      // 每日阅读时长，单位为秒
    uint64_t total_seconds = 0;    // 累计阅读时长，单位为秒
    uint32_t undated_seconds = 0;  // 日期有效后再归入对应日期
};

/** @brief 增量记录阅读时长，复用现有 NVS 持久化任务 */
void BookHomeSnapshotAddDailyReadingSeconds(uint32_t date, uint32_t seconds);

/** @brief 读取当前自然周的内存统计，不在界面线程访问 NVS */
BookHomeSnapshotWeekStats BookHomeSnapshotLoadWeekStats();

struct BookHomeSnapshotAggregateStats {
    bool valid = false;
    uint32_t total_seconds = 0;
    int finished_count = 0;
};

/**
 * @brief 从 NVS 灌入内存（幂等）。
 * 仅允许在内部 RAM 栈任务调用（main_task / book_prog_sync 等），禁止 LVGL 调用。
 */
void BookHomeSnapshotHydrateFromNvs();

/**
 * @brief 进度快照交付后将该书置为最近阅读首项，更新结果异步保存
 * @note 由进度持久化任务调用；正式记录提交或完整临时记录确认可恢复后才视为交付。
 */
void BookHomeSnapshotNoteOpenedBook(const char* book_abs_path);

/** @brief 删书后调用：从 MRU 去掉该路径。落盘异步。 */
void BookHomeSnapshotRemoveBook(const char* book_abs_path);

/**
 * @brief 删书时从聚合统计扣减该书贡献（须在 unlink .pos 之前 Peek 得到）。
 * 无有效快照或 seconds/finished 为 0 时为无操作。落盘异步。
 */
void BookHomeSnapshotSubtractAggregateContribution(uint32_t reading_seconds, bool finished);

/**
 * @brief 读出最近阅读绝对路径（仅内存；未 Hydrate 则空）。
 * 不访问 SD，文件存在性在打开时由后台检查。
 */
size_t BookHomeSnapshotLoadRecentAbsPaths(std::vector<std::string>& out);

/** @brief 仅读取内存中的最近阅读路径，不检查磁盘；无内存记录时返回空串 */
std::string BookHomeSnapshotLoadLatestAbsPath();

/** @brief 仅读内存；未 Hydrate 则 valid=false。 */
BookHomeSnapshotAggregateStats BookHomeSnapshotLoadAggregateStats();

/**
 * @brief 单本进度快照交付后增量更新现有统计，不遍历书库
 * @note 仅统计已提交或确认可恢复的快照，排队状态不作为统计基线。
 */
void BookHomeSnapshotUpdateAggregateProgress(uint32_t old_seconds, bool old_finished, uint32_t seconds, bool finished);
