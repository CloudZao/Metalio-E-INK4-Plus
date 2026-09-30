#pragma once

#include <cstdint>

namespace reader {
namespace book_library_warmup {

/**
 * 开机书库预热（与 library/sync HTTP 解耦）。
 *
 * - 无论是否联网：后台扫盘 → Offer 书库列表缓存 → 全库 Peek .pos → 进度缓存 + 首页聚合
 * - 不发 HTTP；上报由 book_progress_sync 在缓存就绪后按需 POST
 * - DRAM 栈任务；atomic 防重入
 */

void RequestBootWarmup();

bool IsBusy();

enum class Phase : uint8_t {
    kIdle = 0,
    kScan = 1,
    kPeek = 2,
};

struct Progress {
    bool busy = false;
    Phase phase = Phase::kIdle;
    uint32_t done = 0;
    uint32_t total = 0;
};

Progress GetProgress();

/** 进度缓存是否已完成本轮 Peek（空书库亦为 true） */
bool IsProgressCacheReady();

}  // namespace book_library_warmup
}  // namespace reader
