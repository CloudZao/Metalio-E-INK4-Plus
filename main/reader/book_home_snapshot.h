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
 * - 聚合统计：开机 library/sync Peek 时累加落盘，首页直接读，避免再全库扫 .pos
 *
 * Flash/NVS 禁忌：禁止在 LVGL（栈常在 PSRAM）路径直接 nvs_*。
 * 必须先由 DRAM 栈任务 HydrateFromNvs()；UI 只碰内存；落盘走异步 DRAM 任务。
 */

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
 * 过滤已删文件；若有剔除则异步写回 NVS。
 */
size_t LoadRecentAbsPaths(std::vector<std::string>& out);

/** 仅读内存；未 Hydrate 则 valid=false。 */
AggregateStats LoadAggregateStats();

/** 开机同步 Peek 累加后写入内存并异步落盘（DRAM 栈任务写 Flash）。 */
void StoreAggregateStats(uint32_t total_seconds, int finished_count);

/**
 * MRU/删书变更代数：sync Peek 前后不一致则勿 StoreAggregate，
 * 避免「Peek 已计入 → 用户删除已扣减 → Store 又写回含已删书」覆盖。
 */
uint32_t MutationEpoch();

}  // namespace book_home_snapshot
}  // namespace reader
