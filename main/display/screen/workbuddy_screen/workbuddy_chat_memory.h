#pragma once

#include "chat_memory.h"

#include <algorithm>

namespace workbuddy_history {

inline size_t& UsedBytes() {
    static size_t bytes = 0;
    return bytes;
}

inline size_t LimitBytes() {
    return std::min<size_t>(256 * 1024, heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 32);
}

inline size_t ReserveBytes() {
    return std::max<size_t>(2 * 1024 * 1024, heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 4);
}

// 仅由 LVGL 线程使用；统计分配块实际大小，包括容器容量和对齐空间。
template<class T> struct Allocator : ChatAllocator<T> {
    using value_type = T;
    Allocator() noexcept = default;
    template<class U> Allocator(const Allocator<U>&) noexcept {}

    T* allocate(size_t n) {
        const size_t limit = LimitBytes();
        const size_t available = UsedBytes() < limit ? limit - UsedBytes() : 0;
        if (n > available / sizeof(T)) throw std::bad_alloc();
        const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t reserve = ReserveBytes();
        if (free < reserve || n > (free - reserve) / sizeof(T)) throw std::bad_alloc();
        T* p = ChatAllocator<T>::allocate(n);
        const size_t actual = heap_caps_get_allocated_size(p);
        if (actual > available || heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < reserve) {
            ChatAllocator<T>::deallocate(p, n);
            throw std::bad_alloc();
        }
        UsedBytes() += actual;
        return p;
    }

    void deallocate(T* p, size_t n) noexcept {
        UsedBytes() -= heap_caps_get_allocated_size(p);
        ChatAllocator<T>::deallocate(p, n);
    }
};

template<class T, class U> constexpr bool operator==(Allocator<T>, Allocator<U>) { return true; }
template<class T, class U> constexpr bool operator!=(Allocator<T>, Allocator<U>) { return false; }

using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;
template<class T> using Vector = std::vector<T, Allocator<T>>;

}
