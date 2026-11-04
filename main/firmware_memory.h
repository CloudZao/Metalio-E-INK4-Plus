#pragma once

#include <esp_heap_caps.h>
#include <esp_log.h>

inline constexpr uint32_t kFirmwareInternalFloorBytes = 50 * 1024; // 内部 RAM 安全余量

/**
 * @brief 记录内部 RAM 与 DMA 内存的当前、历史最小和最大连续空闲
 * @note 保留历史低水位，用于识别启动、音频和网络阶段的瞬时占用。
 */
inline void FirmwareMemoryCheckpoint(const char* phase) {
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const auto free = heap_caps_get_free_size(internal);
    const auto minimum = heap_caps_get_minimum_free_size(internal);
    ESP_LOGI("MemBudget", "%s internal8 free=%u min=%u largest=%u floor=%u %s",
             phase, unsigned(free), unsigned(minimum),
             unsigned(heap_caps_get_largest_free_block(internal)), unsigned(kFirmwareInternalFloorBytes),
             minimum >= kFirmwareInternalFloorBytes ? "within-floor" : "BELOW-FLOOR");
    ESP_LOGI("MemBudget", "%s internal-DMA free=%u min=%u largest=%u", phase,
             unsigned(heap_caps_get_free_size(dma)),
             unsigned(heap_caps_get_minimum_free_size(dma)),
             unsigned(heap_caps_get_largest_free_block(dma)));
}
