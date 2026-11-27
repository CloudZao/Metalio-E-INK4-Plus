#include "infra/diagnostics/firmware_memory.h"

static void* AllocateWorkspace(size_t size, size_t alignment) {
    void* pointer = heap_caps_aligned_calloc(alignment, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pointer) {
        return pointer;
    }
    constexpr size_t kAllocationSlack = 64;  // 对齐及堆管理开销预留
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    std::lock_guard<std::mutex> lock(firmware_internal_budget_mutex);
    const size_t free = heap_caps_get_free_size(internal);
    if (size <= free && free - size >= kFirmwareInternalFloorBytes + kAllocationSlack) {
        return heap_caps_aligned_calloc(alignment, 1, size, internal);
    }
    return nullptr;
}

/** @brief 分配 JPEG 内部工作区，避免压低内部 RAM 安全余量 */
extern "C" void* __wrap_jpeg_calloc_inner(size_t size) {
    return AllocateWorkspace(size, 16);
}

/** @brief 按库要求对齐 JPEG 工作区，优先使用 PSRAM，内部 RAM 回退遵守预算 */
extern "C" void* __wrap_jpeg_calloc_align_inner(size_t size, int alignment) {
    return AllocateWorkspace(size, static_cast<size_t>(alignment));
}
