#include "list_thumbnail_cache.h"
#include <algorithm>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace reader {
struct CachedListThumbnail {
    std::string path; // 文件标识，仅用于缓存匹配，不输出日志
    int width = 0, height = 0; // 请求的解码边界
    size_t bytes = 0; // 文件版本：长度
    int64_t mtime = 0; // 文件版本：修改时间
    uint64_t used = 0; // 最近使用次序
    RasterImage image; // 缓存持有像素，控件使用独立副本
};
static std::mutex s_mutex;
static std::vector<CachedListThumbnail> s_cache;
static uint64_t s_used = 0;
static constexpr size_t kReserve = 2 * 1024 * 1024; // 缩略图缓存之外保留的 PSRAM 空间
static constexpr size_t kCapacity = 2 * 1024 * 1024; // 书架封面与壁纸缩略图共享的像素缓存上限

static size_t TrimListThumbnails() {
    size_t used = 0;
    for (const auto& item : s_cache) {
        used += item.image.pixels.capacity();
    }
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) + used;
    const size_t budget = available > kReserve ? std::min({kCapacity, available / 2, available - kReserve}) : 0;
    while (!s_cache.empty() && used > budget) {
        const auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                             [](const auto& a, const auto& b) { return a.used < b.used; });
        used -= oldest->image.pixels.capacity();
        s_cache.erase(oldest);
    }
    return budget;
}

bool CanPrefetchListThumbnail(size_t pixels) {
    // 连续空间由实际分配结果判断，避免整堆遍历阻塞扫描中断。
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return pixels && pixels <= kCapacity && free > kReserve && pixels <= (free - kReserve) / 2 &&
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 50 * 1024;
}

void ClearListThumbnailCache() {
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<CachedListThumbnail>().swap(s_cache);
}

bool GetListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime, RasterImage& out) try {
    if (!path) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    TrimListThumbnails();
    for (auto& item : s_cache) {
        if (item.path != path || item.width != width || item.height != height || item.bytes != bytes ||
            item.mtime != mtime) {
            continue;
        }
        if (!CanPrefetchListThumbnail(item.image.pixels.size())) {
            return false;
        }
        out.pixels = item.image.pixels;
        out.width = item.image.width;
        out.height = item.image.height;
        out.BindDsc();
        item.used = ++s_used;
        return true;
    }
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGD("ListThumbCache", "cache copy unavailable");
    return false;
}

bool ListThumbnailUnavailable(const char* path, size_t bytes, int64_t mtime) {
    if (!path) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& item : s_cache) {
        if (item.path == path && item.bytes == bytes && item.mtime == mtime && item.image.empty()) {
            item.used = ++s_used;
            return true;
        }
    }
    return false;
}

void CacheListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime,
                        const RasterImage& image) try {
    if (!path) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    const size_t budget = TrimListThumbnails();
    size_t used = 0;
    for (const auto& item : s_cache) {
        used += item.image.pixels.capacity();
    }
    for (auto it = s_cache.begin(); it != s_cache.end();) {
        if (it->path == path && it->width == width && it->height == height) {
            used -= it->image.pixels.capacity();
            it = s_cache.erase(it);
        } else {
            ++it;
        }
    }
    while (!s_cache.empty() && (used + image.pixels.size() > budget || s_cache.size() >= 48)) {
        const auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                             [](const auto& a, const auto& b) { return a.used < b.used; });
        used -= oldest->image.pixels.capacity();
        s_cache.erase(oldest);
    }
    if (image.pixels.size() > budget) {
        return;
    }
    if (!image.empty() && !CanPrefetchListThumbnail(image.pixels.size())) {
        return;
    }
    if (image.empty() && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 50 * 1024) {
        return;
    }
    s_cache.push_back({path, width, height, bytes, mtime, ++s_used, image});
} catch (const std::bad_alloc&) {
    ESP_LOGD("ListThumbCache", "cache storage unavailable");
}
} // namespace reader
