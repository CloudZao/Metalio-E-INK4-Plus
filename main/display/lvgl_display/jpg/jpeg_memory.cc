#include "firmware_memory.h"

namespace {
void* AllocateWorkspace(size_t size, size_t alignment) {
    constexpr size_t kAllocationSlack = 64; // 对齐及堆管理开销预留
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free = heap_caps_get_free_size(internal);
    if (size <= free && free - size >= kFirmwareInternalFloorBytes + kAllocationSlack) {
        void* pointer = heap_caps_aligned_calloc(alignment, 1, size, internal);
        if (pointer) return pointer;
    }
    return heap_caps_aligned_calloc(alignment, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
} // namespace

/** @brief 分配 JPEG 内部工作区，避免压低内部 RAM 安全余量 */
extern "C" void* __wrap_jpeg_calloc_inner(size_t size) {
    return AllocateWorkspace(size, 16);
}

/** @brief 按库要求对齐 JPEG 工作区，内部 RAM 余量不足时使用 PSRAM */
extern "C" void* __wrap_jpeg_calloc_align_inner(size_t size, int alignment) {
    return AllocateWorkspace(size, static_cast<size_t>(alignment));
}
