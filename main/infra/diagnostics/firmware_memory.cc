#include "firmware_memory.h"

static std::mutex s_memory_mutex;
static std::mutex s_reclaimer_mutex; // 与预算状态锁分离，回调只允许尝试获取模块锁
static FirmwareCacheReclaimer* s_reclaimers = nullptr;
static unsigned s_demands = 0;
static TaskHandle_t s_prefetch_task = nullptr;
static uint32_t s_prefetch_generation = 0; // 最外层执行权的回收代次
static unsigned s_prefetch_depth = 0; // 同一任务的嵌套缓存操作共用执行权
static uint32_t s_reclaim_generation = 0;
static uint32_t s_resume_generation = 0;

bool FirmwarePrefetchAllowed() {
    std::lock_guard<std::mutex> lock(s_memory_mutex);
    return !s_demands && (!s_prefetch_task ||
                          (s_prefetch_task == xTaskGetCurrentTaskHandle() &&
                           s_prefetch_generation == s_reclaim_generation));
}

uint32_t FirmwarePrefetchResumeGeneration() {
    std::lock_guard<std::mutex> lock(s_memory_mutex);
    return s_resume_generation;
}

uint32_t FirmwareMemoryReclaimGeneration() {
    std::lock_guard<std::mutex> lock(s_memory_mutex);
    return s_reclaim_generation;
}

FirmwareCacheReclaimer::FirmwareCacheReclaimer(Callback callback, void* context)
    : callback_(callback), context_(context) {
    std::lock_guard<std::mutex> lock(s_reclaimer_mutex);
    next_ = s_reclaimers;
    s_reclaimers = this;
}

FirmwareCacheReclaimer::~FirmwareCacheReclaimer() {
    std::lock_guard<std::mutex> lock(s_reclaimer_mutex);
    auto** entry = &s_reclaimers;
    while (*entry && *entry != this) {
        entry = &(*entry)->next_;
    }
    if (*entry) {
        *entry = next_;
    }
}

void FirmwareCacheReclaimer::Reclaim() {
    std::lock_guard<std::mutex> lock(s_reclaimer_mutex);
    for (auto* entry = s_reclaimers; entry; entry = entry->next_) {
        entry->callback_(entry->context_);
    }
}

FirmwareMemoryDemand::FirmwareMemoryDemand(size_t bytes, bool enabled) {
    if (!enabled) {
        return;
    }
    bool reclaim = false;
    {
        std::lock_guard<std::mutex> lock(s_memory_mutex);
        if (s_prefetch_task == xTaskGetCurrentTaskHandle()) {
            return;
        }
        ++s_demands;
        active_ = true;
        if (!FirmwarePsramCanAllocate(bytes)) {
            ++s_reclaim_generation;
            reclaim = true;
        }
    }
    if (reclaim) {
        FirmwareCacheReclaimer::Reclaim();
    }
}

FirmwareMemoryDemand::~FirmwareMemoryDemand() {
    if (active_) {
        std::lock_guard<std::mutex> lock(s_memory_mutex);
        if (--s_demands == 0) {
            ++s_resume_generation;
        }
    }
}

FirmwarePrefetchScope::FirmwarePrefetchScope(size_t estimated_bytes, bool enabled) {
    if (!enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_memory_mutex);
    const auto task = xTaskGetCurrentTaskHandle();
    if (s_demands || (s_prefetch_task && s_prefetch_task != task) ||
        !FirmwarePsramCanAllocate(estimated_bytes)) {
        return;
    }
    if (s_prefetch_task && s_prefetch_generation != s_reclaim_generation) {
        return;
    }
    if (!s_prefetch_task) {
        s_prefetch_generation = s_reclaim_generation;
    }
    s_prefetch_task = task;
    ++s_prefetch_depth;
    generation_ = s_reclaim_generation;
    active_ = true;
}

FirmwarePrefetchScope::~FirmwarePrefetchScope() {
    if (active_) {
        std::lock_guard<std::mutex> lock(s_memory_mutex);
        if (--s_prefetch_depth == 0) {
            s_prefetch_task = nullptr;
        }
    }
}

bool FirmwarePrefetchScope::Valid() const {
    std::lock_guard<std::mutex> lock(s_memory_mutex);
    return active_ && !s_demands && generation_ == s_reclaim_generation;
}
