#pragma once

#include <cstdint>

namespace reader {
namespace book_progress_sync {

/**
 * 开机 library/sync HTTP 上报（与扫盘/Peek 解耦）。
 *
 * - 仅在联网时调度；等待 book_library_warmup 进度缓存就绪后组包
 * - 从 book_progress_cache 取进度/时长，不再全库开 .pos
 * - 上报前按 name 去重；单次最多 500 本；不上报 id
 * - 无书发 []；失败只打日志
 */

void RequestBootReport();

bool IsBusy();

enum class Phase : uint8_t {
    kIdle = 0,
    kWaitCache = 1,  // 等待 warmup
    kPost = 2,
};

struct Progress {
    bool busy = false;
    Phase phase = Phase::kIdle;
};

Progress GetProgress();

}  // namespace book_progress_sync
}  // namespace reader
