#pragma once

#include <limits>
#include <new>
#include <string>
#include <vector>
#include <esp_heap_caps.h>

// 语音文本快照和分页缓存只使用 PSRAM，不回退占用 Wi-Fi、I2S DMA 和显示刷新所需的内部 RAM。
template <class T>
struct ChatAllocator {
    using value_type = T;

    /** @brief 创建无独立状态的 PSRAM 分配器 */
    ChatAllocator() noexcept = default;

    /** @brief 从其他元素类型的分配器建立相同的 PSRAM 分配策略 */
    template <class U>
    ChatAllocator(const ChatAllocator<U>&) noexcept {}

    /** @brief 分配指定元素数的 PSRAM，容量溢出或内存不足时抛出 bad_alloc */
    T* allocate(size_t n) {
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        auto* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    /** @brief 释放该策略申请的元素缓冲，不依赖传入的元素数 */
    void deallocate(T* p, size_t) noexcept {
        heap_caps_free(p);
    }
};

/** @brief 所有同策略分配器均可交叉释放缓冲，因此比较结果恒为相等 */
template <class T, class U>
constexpr bool operator==(ChatAllocator<T>, ChatAllocator<U>) {
    return true;
}

/** @brief 同策略分配器不存在不相等的实例 */
template <class T, class U>
constexpr bool operator!=(ChatAllocator<T>, ChatAllocator<U>) {
    return false;
}
using ChatString = std::basic_string<char, std::char_traits<char>, ChatAllocator<char>>;
template <class T>
using ChatVector = std::vector<T, ChatAllocator<T>>;
