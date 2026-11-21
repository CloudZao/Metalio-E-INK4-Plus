#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <initializer_list>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
namespace plus_services {
struct MemorySnapshot {
    std::size_t dma_free = 0, dma_largest = 0, internal_free = 0, psram_free = 0;
};
inline bool DmaBytes(std::size_t descriptors, std::size_t frames, std::size_t slots,
                     std::size_t bytes_per_slot, std::size_t* result) {
    if (!result) return false;
    std::size_t n = 1;
    for (auto value : {descriptors, frames, slots, bytes_per_slot}) {
        if (value == 0 || n > std::numeric_limits<std::size_t>::max() / value) return false;
        n *= value;
    }
    *result = n;
    return true;
}
// Conservative diagnostic, not an allocator guarantee: caller supplies actual
// required largest block and measured reserve for concurrent WiFi/TLS/EPD use.
inline bool HasDmaBudget(const MemorySnapshot& m, std::size_t total,
                         std::size_t largest_request, std::size_t reserve) {
    return largest_request <= m.dma_largest && total <= m.dma_free &&
           reserve <= m.dma_free - total;
}
#ifdef ESP_PLATFORM
inline MemorySnapshot ReadMemorySnapshot() {
    return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM)};
}
#endif
} // namespace plus_services

