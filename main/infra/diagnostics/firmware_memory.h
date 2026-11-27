#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

inline constexpr uint32_t kFirmwareInternalFloorBytes = 50 * 1024;  // 内部 RAM 安全余量

inline constexpr size_t kFirmwarePsramReserveBytes = 2 * 1024 * 1024; // 为解码、绘制及其他业务保留的 PSRAM

/** @brief 查询是否允许开始下一项可选预取。 */
bool FirmwarePrefetchAllowed();

/** @brief 获取公共操作结束的代次，供现有调度入口按需恢复预取。 */
uint32_t FirmwarePrefetchResumeGeneration();

/** @brief 获取回收代次，旧代次的可选结果不得重新填入缓存。 */
uint32_t FirmwareMemoryReclaimGeneration();

/** @brief 登记缓存回收入口；回调只能尝试释放闲置资源，不等待模块锁或执行公共申请。 */
class FirmwareCacheReclaimer {
public:
    using Callback = void (*)(void*);

    /** @brief 注册随对象生存的回收入口，回调不得注销注册项。 */
    FirmwareCacheReclaimer(Callback callback, void* context);

    /** @brief 等待正在执行的回调结束并注销入口。 */
    ~FirmwareCacheReclaimer();

    FirmwareCacheReclaimer(const FirmwareCacheReclaimer&) = delete;
    FirmwareCacheReclaimer& operator=(const FirmwareCacheReclaimer&) = delete;

private:
    friend class FirmwareMemoryDemand;
    static void Reclaim();
    Callback callback_; // 非阻塞回收函数
    void* context_; // 注册者保证在注销前有效
    FirmwareCacheReclaimer* next_ = nullptr; // 由注册表锁保护
};

/** @brief 在公共大内存操作期间暂停预取，余量不足时通知回收；不保证实际分配成功。 */
class FirmwareMemoryDemand {
public:
    /** @brief 按峰值粗估请求空间，预取任务内部调用不登记公共需求。 */
    explicit FirmwareMemoryDemand(size_t bytes, bool enabled = true);

    /** @brief 解除本次公共需求，最后一个需求结束后允许预取恢复。 */
    ~FirmwareMemoryDemand();

    FirmwareMemoryDemand(const FirmwareMemoryDemand&) = delete;
    FirmwareMemoryDemand& operator=(const FirmwareMemoryDemand&) = delete;

private:
    bool active_ = false; // 是否持有公共需求计数
};

/** @brief 串行执行一项可选预取，按实际堆余量准入，不预留精确额度。 */
class FirmwarePrefetchScope {
public:
    /** @brief 尝试取得执行权，失败时调用方应延后当前预取。 */
    explicit FirmwarePrefetchScope(size_t estimated_bytes, bool enabled = true);

    /** @brief 归还执行权。 */
    ~FirmwarePrefetchScope();

    /** @brief 查询执行权及回收代次是否仍有效。 */
    bool Valid() const;

    FirmwarePrefetchScope(const FirmwarePrefetchScope&) = delete;
    FirmwarePrefetchScope& operator=(const FirmwarePrefetchScope&) = delete;

private:
    bool active_ = false; // 本对象是否取得执行权
    uint32_t generation_ = 0; // 开始执行时的回收代次
};

/** @brief 按剩余内存和已持有缓存计算可选缓存预算，至少保留一半可用空间。 */
inline size_t FirmwarePsramCacheBudget(size_t used) {
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kFirmwareInternalFloorBytes) {
        return 0;
    }
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) + used;
    return available > kFirmwarePsramReserveBytes
        ? std::min(available / 2, available - kFirmwarePsramReserveBytes) : 0;
}

/** @brief 判断分配及后续操作能否保留内存余量，连续空间由实际分配结果确认。 */
inline bool FirmwarePsramCanAllocate(size_t bytes, size_t working = 0) {
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return free > kFirmwarePsramReserveBytes && working <= free - kFirmwarePsramReserveBytes &&
           bytes <= free - kFirmwarePsramReserveBytes - working &&
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= kFirmwareInternalFloorBytes;
}

inline std::mutex firmware_internal_budget_mutex;  // 保护任务创建、JPEG 工作区及图像和正文的内部 RAM 回退分配

/** @brief 供统一任务创建入口估算内部 RAM 余量；调用方须串行保护检查与创建。 */
inline bool FirmwareTaskMemoryAvailable(size_t stack_bytes, uint32_t stack_caps) {
    const size_t internal_bytes = sizeof(StaticTask_t) +
        ((stack_caps & MALLOC_CAP_SPIRAM) != 0 ? 0 : stack_bytes);
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
           kFirmwareInternalFloorBytes + internal_bytes;
}

/**
 * @brief 记录内部 RAM 与 DMA 内存的当前、历史最小和最大连续空闲
 * @note 保留历史低水位，用于识别启动、音频和网络阶段的瞬时占用。
 */
inline void FirmwareMemoryCheckpoint(const char* phase) {
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const auto free = heap_caps_get_free_size(internal);
    const auto minimum = heap_caps_get_minimum_free_size(internal);
    ESP_LOGI("MemBudget", "%s internal8 free=%u min=%u largest=%u floor=%u %s", phase, unsigned(free),
             unsigned(minimum), unsigned(heap_caps_get_largest_free_block(internal)),
             unsigned(kFirmwareInternalFloorBytes),
             minimum >= kFirmwareInternalFloorBytes ? "within-floor" : "BELOW-FLOOR");
    ESP_LOGI("MemBudget", "%s internal-DMA free=%u min=%u largest=%u", phase, unsigned(heap_caps_get_free_size(dma)),
             unsigned(heap_caps_get_minimum_free_size(dma)), unsigned(heap_caps_get_largest_free_block(dma)));
}
