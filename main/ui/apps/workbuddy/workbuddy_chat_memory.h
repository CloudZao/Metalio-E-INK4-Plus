#pragma once

#include <algorithm>
#include "business/conversation/chat_memory.h"

inline size_t& WorkBuddyHistoryUsedBytes() {
    static size_t bytes = 0;
    return bytes;
}

inline size_t WorkBuddyHistoryLimitBytes() {
    return std::min<size_t>(256 * 1024, heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 32);
}

inline size_t WorkBuddyHistoryReserveBytes() {
    return std::max<size_t>(2 * 1024 * 1024, heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 4);
}

// 仅由 LVGL 线程使用；统计分配块实际大小，包括容器容量和对齐空间。
template <class T>
struct WorkBuddyHistoryAllocator : ChatAllocator<T> {
    using value_type = T;
    WorkBuddyHistoryAllocator() noexcept = default;

    template <class U>
    WorkBuddyHistoryAllocator(const WorkBuddyHistoryAllocator<U>&) noexcept {}

    T* allocate(size_t n) {
        const size_t limit = WorkBuddyHistoryLimitBytes();
        const size_t available = WorkBuddyHistoryUsedBytes() < limit ? limit - WorkBuddyHistoryUsedBytes() : 0;
        if (n > available / sizeof(T)) {
            throw std::bad_alloc();
        }
        const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t reserve = WorkBuddyHistoryReserveBytes();
        if (free < reserve || n > (free - reserve) / sizeof(T)) {
            throw std::bad_alloc();
        }
        T* p = ChatAllocator<T>::allocate(n);
        const size_t actual = heap_caps_get_allocated_size(p);
        if (actual > available || heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < reserve) {
            ChatAllocator<T>::deallocate(p, n);
            throw std::bad_alloc();
        }
        WorkBuddyHistoryUsedBytes() += actual;
        return p;
    }

    void deallocate(T* p, size_t n) noexcept {
        WorkBuddyHistoryUsedBytes() -= heap_caps_get_allocated_size(p);
        ChatAllocator<T>::deallocate(p, n);
    }
};

template <class T, class U>
constexpr bool operator==(WorkBuddyHistoryAllocator<T>, WorkBuddyHistoryAllocator<U>) {
    return true;
}

template <class T, class U>
constexpr bool operator!=(WorkBuddyHistoryAllocator<T>, WorkBuddyHistoryAllocator<U>) {
    return false;
}

using WorkBuddyHistoryString = std::basic_string<char, std::char_traits<char>, WorkBuddyHistoryAllocator<char>>;
template <class T>
using WorkBuddyHistoryVector = std::vector<T, WorkBuddyHistoryAllocator<T>>;
