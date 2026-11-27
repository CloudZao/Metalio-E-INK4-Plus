#include <cstddef>
#include <esp_heap_caps.h>

/** @brief 将LVGL对象内存分配到PSRAM，保留内部DMA内存 */
extern "C" void* __wrap_lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** @brief 在PSRAM中调整LVGL对象内存容量 */
extern "C" void* __wrap_lv_realloc_core(void* memory, size_t size) {
    return heap_caps_realloc(memory, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
